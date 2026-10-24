/*
 * MtkAwdlCap.cpp — rz616-m3i-1 AWDL capability probe. See MtkAwdlCap.hpp.
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
 * GPLv2.
 */
#include "AirportItlwmV2.hpp"
#include "MtkAwdlCap.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/OSAtomic.h>
#include <sys/proc.h>
#include <string.h>

static_assert(sizeof(struct apple80211_virt_if_create_data) == 32,
    "IO80211Family 1200.13.1 requires req_len == 0x20 for VIRTUAL_IF_CREATE");
static_assert(sizeof(struct apple80211_virt_if_create_data) <=
    sizeof(((struct mtk_awdlcap_rec *)0)->data), "vif record too small");

/* Log at most MTK_AWDLCAP_LOG_BURST lines per kind per 60 s window. */
#define MTK_AWDLCAP_LOG_BURST     8
#define MTK_AWDLCAP_LOG_WINDOW_US (60ull * 1000 * 1000)

static IOSimpleLock * volatile gLock;   /* kext lifetime, see above */
static uint32_t gEnabled;     /* default off; never persisted */
static uint64_t gSetCalls, gCapsCalls, gCapsBitSet, gVifCalls, gSuppressed;
static uint64_t gRecTotal;
static struct mtk_awdlcap_rec gRecs[MTK_AWDLCAP_RECS];
static uint64_t gLogWindowStart[3];
static uint32_t gLogInWindow[3];
static uint32_t gMarkerLogged;

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
    if (kind >= 3)
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
        IOLog("AirportMtkWlan: awdlcap probe rz616-m3i-1 (switch default off)\n");
    }
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
    memcpy(out->recs, gRecs, sizeof(gRecs));
    IOSimpleLockUnlock(lk);
    return true;
}
