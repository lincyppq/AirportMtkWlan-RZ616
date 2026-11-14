/*
 * MtkAwdlCap.cpp — rz616-m3i-2 AWDL capability probe + awdl0 creation.
 * See MtkAwdlCap.hpp.
 *
 * Locking: one IOSimpleLock (a spin lock; preemption is disabled while it is
 * held). Every caller runs in thread context (ioctl / user client paths), so
 * interrupts are left enabled. Only fixed-size memcpy/memset happen under the
 * lock: no allocation, no IOLog, no proc_* calls, nothing that can block.
 * Lifetime: the lock belongs to the kext, not to a driver instance. The first
 * AirportItlwm::start() installs it (compare-and-swap, so racing instances
 * cannot both install one); no instance ever frees it. It is released only by
 * the module destructor when the kext unloads, which cannot happen while any
 * instance exists.
 *
 * Interface creation (m3i-2) never goes through a virtual table: the
 * IO80211VirtualInterface header that ships with the driver does not match the
 * running kernel's vtable order (measured: init is at +0xe78, not where the
 * header puts it). Everything below calls the exported symbols directly.
 * GPLv2.
 */
#include "AirportItlwmV2.hpp"
#include "MtkAwdlCap.hpp"
#include "MtkAwdlInterface.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSMetaClass.h>
#include <sys/proc.h>
#include <string.h>

static_assert(sizeof(struct apple80211_virt_if_create_data) == 32,
    "IO80211Family 1200.13.1 requires req_len == 0x20 for VIRTUAL_IF_CREATE");
static_assert(sizeof(struct apple80211_virt_if_create_data) <=
    sizeof(((struct mtk_awdlcap_rec *)0)->data), "vif record too small");
static_assert(sizeof(struct mtk_awdlcap_vifres) <=
    sizeof(((struct mtk_awdlcap_rec *)0)->data), "vif result too big for a record");

/* Log at most MTK_AWDLCAP_LOG_BURST lines per kind per 60 s window. */
#define MTK_AWDLCAP_LOG_BURST     8
#define MTK_AWDLCAP_LOG_WINDOW_US (60ull * 1000 * 1000)
/* Kinds that may log; keep in sync with mtk_awdlcap_read()/may_log(). */
#define MTK_AWDLCAP_KINDS         5

/* APPLE80211_VIF_AWDL (apple80211_var.h, __IO80211_TARGET >= __MAC_13_0). */
#define MTK_AWDL_VIF_ROLE         6u
/* BSD name prefix handed to IO80211VirtualInterface::init; IO80211Family does
 * the same with "ap" for role 7 (SOFT_AP), which yields ap0/ap1. */
#define MTK_AWDL_VIF_NAME         "awdl"

/*
 * Direct symbol calls. On x86_64 a member function takes `this` as the first
 * argument, so declaring them as C functions with an explicit first parameter
 * binds to the same code the kernel's own callers reach.
 */
extern "C" {
/* IO80211VirtualInterface::init(IO80211Controller*, ether_addr*, UInt, char const*) */
bool mtk_vif_init(void *vif, void *controller, void *mac, unsigned int role,
                  const char *name)
    __asm__("__ZN23IO80211VirtualInterface4initEP17IO80211ControllerP10ether_addrjPKc");
/* IO80211VirtualInterface::attach(IOService*) — the family's override, which
 * does its own setup and then calls IOService::attach(). */
bool mtk_vif_attach(void *vif, void *provider)
    __asm__("__ZN23IO80211VirtualInterface6attachEP9IOService");
/* IO80211VirtualInterface::detach(IOService*) */
bool mtk_vif_detach(void *vif, void *provider)
    __asm__("__ZN23IO80211VirtualInterface6detachEP9IOService");
/* IOService::registerService(unsigned int) */
bool mtk_vif_register(void *service, unsigned int options)
    __asm__("__ZN9IOService15registerServiceEj");
/* OSObject::release() is const (`__ZNK8OSObject7releaseEv`); call it through
 * the class instead of an asm label so the mangling stays correct. */
static void mtk_obj_release(OSObject *o)
{
    if (o != NULL)
        o->release();
}
/* IO80211VirtualInterface::metaClass (data). AppleBCMWLANCore binds to the
 * same symbol, so it is exported for kexts. */
extern OSMetaClass *mtk_vif_metaClass
    __asm__("__ZN23IO80211VirtualInterface9metaClassE");
}

static IOSimpleLock * volatile gLock;   /* kext lifetime, see above */
static uint32_t gEnabled;     /* default off; never persisted */
static uint64_t gSetCalls, gCapsCalls, gCapsBitSet, gVifCalls, gSuppressed;
static uint64_t gRecTotal;
static struct mtk_awdlcap_rec gRecs[MTK_AWDLCAP_RECS];
static uint64_t gLogWindowStart[MTK_AWDLCAP_KINDS];
static uint32_t gLogInWindow[MTK_AWDLCAP_KINDS];
static uint32_t gMarkerLogged;

/* m3i-2 state. gController/gVif are published under gLock. */
static void * volatile gController;
static void * volatile gVif;
static uint64_t gVifOk, gVifFail;
static uint32_t gVifRole;
static uint8_t  gVifMac[6];
static uint16_t gVifMtu;                /* 0 = not read back */
static char     gVifName[IFNAMSIZ];
static struct apple80211_virt_if_create_data gLastReq;
static uint32_t gLastReqValid;

void
mtk_awdlcap_init(void)
{
    IOSimpleLock *l;

    if (__atomic_load_n(&gLock, __ATOMIC_ACQUIRE) != NULL)
        return;
    l = IOSimpleLockAlloc();
    if (l == NULL) {
        IOLog("AirportMtkWlan: awdlcap lock alloc failed; probe disabled\n");
        return;
    }
    if (!OSCompareAndSwapPtr(NULL, l, (void * volatile *)&gLock))
        IOSimpleLockFree(l);    /* another instance installed one first */
}

/* Kext unload only (__mod_term_func); every driver instance is gone by then. */
__attribute__((destructor)) static void
mtk_awdlcap_unload(void)
{
    IOSimpleLock *l = gLock;

    gLock = NULL;
    mtk_awdlcap_vif_drop();
    if (l != NULL)
        IOSimpleLockFree(l);
}

/* NULL until mtk_awdlcap_init succeeded; then constant for the kext lifetime. */
static IOSimpleLock *
lockp(void)
{
    return __atomic_load_n(&gLock, __ATOMIC_ACQUIRE);
}

static uint64_t
now_us(void)
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}

/* Called with the lock held. Returns true when this line may be logged. */
static bool
may_log(uint32_t kind, uint64_t ts)
{
    if (kind >= MTK_AWDLCAP_KINDS)
        return false;
    if (ts - gLogWindowStart[kind] > MTK_AWDLCAP_LOG_WINDOW_US) {
        gLogWindowStart[kind] = ts;
        gLogInWindow[kind] = 0;
    }
    if (gLogInWindow[kind] >= MTK_AWDLCAP_LOG_BURST) {
        gSuppressed++;
        return false;
    }
    gLogInWindow[kind]++;
    return true;
}

/* Called with the lock held. */
static struct mtk_awdlcap_rec *
next_rec(uint32_t kind, uint64_t ts, int pid, const char *comm)
{
    struct mtk_awdlcap_rec *r = &gRecs[gRecTotal % MTK_AWDLCAP_RECS];
    gRecTotal++;
    memset(r, 0, sizeof(*r));
    r->ts_us = ts;
    r->kind = kind;
    r->pid = pid;
    memcpy(r->comm, comm, sizeof(r->comm));
    r->enabled = gEnabled;
    return r;
}

/* Called without the lock. */
static void
marker(void)
{
    if (!gMarkerLogged) {
        gMarkerLogged = 1;
        IOLog("AirportMtkWlan: awdlcap probe rz616-m3i-2 (switch default off, "
              "role-6 VIRTUAL_IF_CREATE builds awdl0 when on)\n");
    }
}

void
mtk_awdlcap_bind_controller(void *controller)
{
    if (__atomic_load_n(&gController, __ATOMIC_ACQUIRE) == NULL && controller != NULL)
        gController = controller;
}

void *
mtk_awdlcap_controller(void)
{
    return __atomic_load_n(&gController, __ATOMIC_ACQUIRE);
}

void
mtk_awdlcap_on_card_caps(uint8_t *caps, uint32_t len)
{
    char comm[16] = {0};
    uint64_t ts;
    bool log;
    uint32_t sw;
    uint8_t c3, c4;
    int pid;

    IOSimpleLock *lk = lockp();

    if (lk == NULL || caps == NULL || len <= 4)
        return;
    pid = proc_selfpid();
    proc_selfname(comm, sizeof(comm));
    ts = now_us();

    IOSimpleLockLock(lk);
    sw = gEnabled;
    if (sw)
        caps[4] |= 0x01;
    c3 = caps[3];
    c4 = caps[4];
    gCapsCalls++;
    if (c4 & 0x01)
        gCapsBitSet++;
    struct mtk_awdlcap_rec *r = next_rec(MTK_AWDLCAP_KIND_CAPS, ts, pid, comm);
    r->ret = len < sizeof(r->data) ? len : sizeof(r->data);
    memcpy(r->data, caps, r->ret);
    log = may_log(MTK_AWDLCAP_KIND_CAPS, ts);
    IOSimpleLockUnlock(lk);

    marker();
    if (log)
        IOLog("AirportMtkWlan: awdlcap caps pid=%d comm=%s sw=%u cap3=%02x cap4=%02x\n",
            pid, comm, sw, c3, c4);
}

void
mtk_awdlcap_on_vif_create(const struct apple80211_virt_if_create_data *d,
    IOReturn ret)
{
    char comm[16] = {0};
    char name[IFNAMSIZ + 1] = {0};
    struct apple80211_virt_if_create_data v;
    uint64_t ts;
    bool log;
    uint32_t sw;
    int pid;

    IOSimpleLock *lk = lockp();

    if (lk == NULL)
        return;
    pid = proc_selfpid();
    proc_selfname(comm, sizeof(comm));
    ts = now_us();
    memset(&v, 0, sizeof(v));
    if (d != NULL)
        memcpy(&v, d, sizeof(v));
    memcpy(name, v.bsd_name, IFNAMSIZ);

    IOSimpleLockLock(lk);
    sw = gEnabled;
    gVifCalls++;
    if (d != NULL) {
        /* Remembered so the awdlcap tool can replay the same request by hand. */
        memcpy(&gLastReq, &v, sizeof(gLastReq));
        gLastReqValid = 1;
    }
    struct mtk_awdlcap_rec *r = next_rec(MTK_AWDLCAP_KIND_VIF_CREATE, ts, pid, comm);
    r->ret = (uint32_t)ret;
    if (d != NULL)
        memcpy(r->data, &v, sizeof(v));
    log = may_log(MTK_AWDLCAP_KIND_VIF_CREATE, ts);
    IOSimpleLockUnlock(lk);

    marker();
    if (log)
        IOLog("AirportMtkWlan: awdlcap vif_create pid=%d comm=%s sw=%u null=%d "
            "ver=%u role=%u mac=%02x:%02x:%02x:%02x:%02x:%02x unk1=0x%04x "
            "name=%s ret=0x%x\n",
            pid, comm, sw, d == NULL, v.version, v.role,
            v.mac[0], v.mac[1], v.mac[2], v.mac[3], v.mac[4], v.mac[5],
            v.unk1, name, ret);
}

/* ---------------------------------------------------------------- m3i-2 -- */

/* Called without the lock. Records one step of the bring-up. */
static void
vif_note(uint32_t step, IOReturn ret, const void *mac, const char *why)
{
    char comm[16] = {0};
    struct mtk_awdlcap_vifres v;
    IOSimpleLock *lk = lockp();
    uint64_t ts;
    int pid;
    bool log;

    if (lk == NULL)
        return;
    pid = proc_selfpid();
    proc_selfname(comm, sizeof(comm));
    ts = now_us();

    memset(&v, 0, sizeof(v));
    v.step = step;
    v.ret = (uint32_t)ret;
    v.role = MTK_AWDL_VIF_ROLE;
    v.mtu = gVifMtu;
    if (mac != NULL)
        memcpy(v.mac, mac, sizeof(v.mac));

    IOSimpleLockLock(lk);
    struct mtk_awdlcap_rec *r = next_rec(MTK_AWDLCAP_KIND_VIF_RESULT, ts, pid, comm);
    r->ret = (uint32_t)ret;
    memcpy(r->data, &v, sizeof(v));
    log = may_log(MTK_AWDLCAP_KIND_VIF_RESULT, ts);
    IOSimpleLockUnlock(lk);

    if (log)
        IOLog("AirportMtkWlan: awdlcap vif step=%u ret=0x%x %s\n",
            step, (unsigned)ret, why != NULL ? why : "");
}

void
mtk_awdlcap_note(uint32_t step, IOReturn ret, const void *mac, const char *why)
{
    vif_note(step, ret, mac, why);
}

IOReturn
mtk_awdlcap_vif_create(void *controller, const struct apple80211_virt_if_create_data *d)
{
    struct apple80211_virt_if_create_data v;
    IOSimpleLock *lk = lockp();
    OSObject *obj;
    uint32_t sw;
    uint32_t valid;

    if (lk == NULL)
        return kIOReturnNotReady;

    IOSimpleLockLock(lk);
    sw = gEnabled;
    valid = gLastReqValid;
    if (d != NULL) {
        memcpy(&v, d, sizeof(v));
    } else if (valid) {
        memcpy(&v, &gLastReq, sizeof(v));
    } else {
        memset(&v, 0, sizeof(v));
        v.role = MTK_AWDL_VIF_ROLE;
        v.mac[0] = 0x02;    /* manual bring-up without a recorded request */
    }
    IOSimpleLockUnlock(lk);

    if (gVif != NULL) {
        gVifOk++;
        return kIOReturnSuccess;        /* idempotent: one interface object */
    }
    /* Switch off or a role we do not implement: no allocation at all, exactly
     * like m3i-1. The native gate does not bypass this. */
    if (!sw || v.role != MTK_AWDL_VIF_ROLE)
        return kIOReturnUnsupported;
    if (mtk_awdl_native_gate_on()) {
        void *iface = NULL;
        IOReturn nr = mtk_awdl_native_bringup(controller, v.mac, v.role, &iface);
        if (nr == kIOReturnSuccess && iface != NULL) {
            IOSimpleLockLock(lk);
            gVif = iface;
            gVifRole = v.role;
            memcpy(gVifMac, v.mac, sizeof(gVifMac));
            memcpy(gVifName, "object", 7);
            gVifOk++;
            IOSimpleLockUnlock(lk);
            return kIOReturnSuccess;
        }
        gVifFail++;
        return nr == kIOReturnSuccess ? kIOReturnError : nr;
    }
    if (controller == NULL) {
        gVifFail++;
        vif_note(MTK_AWDLCAP_STEP_ALLOC, kIOReturnNotReady, v.mac, "no controller");
        return kIOReturnNotReady;
    }

    obj = mtk_vif_metaClass != NULL ? mtk_vif_metaClass->alloc() : NULL;
    if (obj == NULL) {
        gVifFail++;
        vif_note(MTK_AWDLCAP_STEP_ALLOC, kIOReturnNoMemory, v.mac, "alloc");
        return kIOReturnNoMemory;
    }
    vif_note(MTK_AWDLCAP_STEP_ALLOC, kIOReturnSuccess, v.mac, "alloc");

    if (!mtk_vif_init(obj, controller, v.mac, MTK_AWDL_VIF_ROLE, MTK_AWDL_VIF_NAME)) {
        gVifFail++;
        vif_note(MTK_AWDLCAP_STEP_INIT, kIOReturnError, v.mac, "init");
        mtk_obj_release(obj);
        return kIOReturnError;
    }
    vif_note(MTK_AWDLCAP_STEP_INIT, kIOReturnSuccess, v.mac, "init");

    if (!mtk_vif_attach(obj, controller)) {
        gVifFail++;
        vif_note(MTK_AWDLCAP_STEP_ATTACH, kIOReturnError, v.mac, "attach");
        mtk_obj_release(obj);
        return kIOReturnError;
    }
    vif_note(MTK_AWDLCAP_STEP_ATTACH, kIOReturnSuccess, v.mac, "attach");

    if (!mtk_vif_register(obj, 0)) {
        /* Not fatal on its own: the interface may still be published by the
         * family's own path. Record it and keep going. */
        vif_note(MTK_AWDLCAP_STEP_REGISTER, kIOReturnError, v.mac, "register");
    } else {
        vif_note(MTK_AWDLCAP_STEP_REGISTER, kIOReturnSuccess, v.mac, "register");
    }

    IOSimpleLockLock(lk);
    gVif = obj;
    gVifRole = MTK_AWDL_VIF_ROLE;
    memcpy(gVifMac, v.mac, sizeof(gVifMac));
    memcpy(gVifName, MTK_AWDL_VIF_NAME, sizeof(MTK_AWDL_VIF_NAME));
    gVifOk++;
    IOSimpleLockUnlock(lk);

    vif_note(MTK_AWDLCAP_STEP_DONE, kIOReturnSuccess, v.mac, "created");
    IOLog("AirportMtkWlan: awdlcap built %s (role=%u mac=%02x:%02x:%02x:%02x:%02x:%02x)\n",
        MTK_AWDL_VIF_NAME, MTK_AWDL_VIF_ROLE,
        v.mac[0], v.mac[1], v.mac[2], v.mac[3], v.mac[4], v.mac[5]);
    return kIOReturnSuccess;
}

void
mtk_awdlcap_vif_drop(void)
{
    void *obj;
    void *ctrl = mtk_awdlcap_controller();

    if (lockp() == NULL && gVif == NULL)
        return;
    obj = gVif;
    gVif = NULL;
    if (obj == NULL)
        return;
    if (OSDynamicCast(MtkAwdlInterface, static_cast<OSObject *>(obj)) != NULL) {
        mtk_awdl_native_teardown(ctrl, obj);
        vif_note(MTK_AWDLCAP_STEP_DROP, kIOReturnSuccess, NULL, "native-dropped");
        return;
    }
    if (ctrl != NULL)
        mtk_vif_detach(obj, ctrl);
    mtk_obj_release((OSObject *)obj);
    vif_note(MTK_AWDLCAP_STEP_DROP, kIOReturnSuccess, NULL, "dropped");
}

bool
mtk_awdlcap_set(uint32_t enabled)
{
    char comm[16] = {0};
    uint32_t old, val = enabled ? 1 : 0;
    uint64_t ts;
    int pid;

    IOSimpleLock *lk = lockp();

    if (lk == NULL)
        return false;
    pid = proc_selfpid();
    proc_selfname(comm, sizeof(comm));
    ts = now_us();

    IOSimpleLockLock(lk);
    old = gEnabled;
    gEnabled = val;
    gSetCalls++;
    struct mtk_awdlcap_rec *r = next_rec(MTK_AWDLCAP_KIND_SET, ts, pid, comm);
    r->ret = old;
    IOSimpleLockUnlock(lk);

    marker();
    IOLog("AirportMtkWlan: awdlcap switch %u -> %u (pid=%d comm=%s)\n",
        old, val, pid, comm);
    return true;
}

bool
mtk_awdlcap_read(struct mtk_awdlcap_state *out)
{
    uint64_t ts;

    IOSimpleLock *lk = lockp();

    if (lk == NULL)
        return false;
    ts = now_us();

    IOSimpleLockLock(lk);
    memset(out, 0, sizeof(*out));
    out->abi = MTK_AWDLCAP_ABI;
    out->enabled = gEnabled;
    out->set_calls = gSetCalls;
    out->caps_calls = gCapsCalls;
    out->caps_calls_bit_set = gCapsBitSet;
    out->vif_create_calls = gVifCalls;
    out->log_suppressed = gSuppressed;
    out->rec_total = gRecTotal;
    out->now_us = ts;
    out->vif_ok = gVifOk;
    out->vif_fail = gVifFail;
    out->vif_live = gVif != NULL;
    out->vif_role = gVifRole;
    out->vif_mtu = gVifMtu;
    memcpy(out->vif_mac, gVifMac, sizeof(out->vif_mac));
    memcpy(out->vif_name, gVifName, sizeof(out->vif_name));
    memcpy(out->recs, gRecs, sizeof(gRecs));
    IOSimpleLockUnlock(lk);
    return true;
}
