/*
 * awdlcap — rz616-m3i-2 AWDL capability probe / awdl0 bring-up control.
 *
 * Build:
 *   ./build.sh awdlcap   (from the repository root) -> tools/awdlcap/awdlcap
 *
 * Needs root and a kext built with MTK_AWDL_CAP_PROBE, booted with -mtkawdltx.
 *
 *   awdlcap [status]   print the switch, counters, vif state and record ring
 *   awdlcap on         set capabilities[4] bit0 in CARD_CAPABILITIES replies
 *   awdlcap off        stop setting it (driver default)
 *   awdlcap create     m3i-2: build awdl0 by hand (replays the last request)
 *   awdlcap drop       m3i-2: detach and release the interface built above
 *
 * The switch lives only in kernel memory: a reboot or kext reload turns it off.
 * Turning it off changes only what the driver answers from then on; it does not
 * undo anything user space did while it was on.
 *
 * GPLv2.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <ClientKit/AwdlRaw.h>

static io_connect_t conn_open(void) {
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AirportItlwm"));
    io_connect_t c = 0;
    if (!s) { fprintf(stderr, "AirportItlwm not found (is the kext loaded?)\n"); return 0; }
    kern_return_t kr = IOServiceOpen(s, mach_task_self(), MTK_AWDLRAW_USERCLIENT_TYPE, &c);
    IOObjectRelease(s);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceOpen(type=AWDT): 0x%x%s\n", kr,
            geteuid() != 0 ? " (need root: sudo)" : "");
        return 0;
    }
    return c;
}

static void fmt_wall(uint64_t ts_us, uint64_t now_us, char *buf, size_t n) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t ago = (int64_t)(now_us - ts_us);
    int64_t wall_us = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec - ago;
    time_t sec = (time_t)(wall_us / 1000000);
    struct tm tm;
    localtime_r(&sec, &tm);
    size_t k = strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(buf + k, n - k, ".%03lld", (long long)(wall_us % 1000000) / 1000);
}

static const char *step_name(uint32_t step) {
    switch (step) {
    case MTK_AWDLCAP_STEP_ALLOC:    return "alloc";
    case MTK_AWDLCAP_STEP_INIT:     return "init";
    case MTK_AWDLCAP_STEP_ATTACH:   return "attach";
    case MTK_AWDLCAP_STEP_REGISTER: return "register";
    case MTK_AWDLCAP_STEP_DONE:     return "done";
    case MTK_AWDLCAP_STEP_DROP:     return "drop";
    default:                        return "?";
    }
}

static void print_state(const struct mtk_awdlcap_state *st) {
    printf("abi=%u switch=%s set_calls=%llu caps_calls=%llu caps_bit_set=%llu "
           "vif_create_calls=%llu log_suppressed=%llu records=%llu\n",
        st->abi, st->enabled ? "ON" : "off",
        (unsigned long long)st->set_calls, (unsigned long long)st->caps_calls,
        (unsigned long long)st->caps_calls_bit_set, (unsigned long long)st->vif_create_calls,
        (unsigned long long)st->log_suppressed, (unsigned long long)st->rec_total);

    printf("vif: live=%u ok=%llu fail=%llu role=%u mac=%02x:%02x:%02x:%02x:%02x:%02x "
           "mtu=%u name=\"%s\"\n",
        st->vif_live, (unsigned long long)st->vif_ok, (unsigned long long)st->vif_fail,
        st->vif_role, st->vif_mac[0], st->vif_mac[1], st->vif_mac[2],
        st->vif_mac[3], st->vif_mac[4], st->vif_mac[5], st->vif_mtu,
        st->vif_name);

    uint64_t total = st->rec_total, n = total < MTK_AWDLCAP_RECS ? total : MTK_AWDLCAP_RECS;
    for (uint64_t i = total - n; i < total; i++) {
        const struct mtk_awdlcap_rec *r = &st->recs[i % MTK_AWDLCAP_RECS];
        char when[40], comm[17] = {0};
        memcpy(comm, r->comm, 16);
        fmt_wall(r->ts_us, st->now_us, when, sizeof(when));
        printf("#%llu %s up=%llu.%06llu pid=%d comm=%s sw=%u ",
            (unsigned long long)i, when, (unsigned long long)(r->ts_us / 1000000),
            (unsigned long long)(r->ts_us % 1000000), r->pid, comm, r->enabled);
        if (r->kind == MTK_AWDLCAP_KIND_CAPS) {
            printf("CARD_CAPABILITIES cap[4]=%02x bytes=", r->data[4]);
            for (uint32_t k = 0; k < r->ret && k < sizeof(r->data); k++) printf("%02x", r->data[k]);
            printf("\n");
        } else if (r->kind == MTK_AWDLCAP_KIND_VIF_CREATE) {
            uint32_t ver, role; uint16_t unk1; char name[17] = {0};
            memcpy(&ver, r->data, 4); memcpy(&unk1, r->data + 10, 2);
            memcpy(&role, r->data + 12, 4); memcpy(name, r->data + 16, 16);
            printf("VIRTUAL_IF_CREATE ret=0x%x ver=%u mac=%02x:%02x:%02x:%02x:%02x:%02x "
                   "unk1=0x%04x role=%u name=\"%s\" raw=",
                r->ret, ver, r->data[4], r->data[5], r->data[6], r->data[7], r->data[8],
                r->data[9], unk1, role, name);
            for (int k = 0; k < 32; k++) printf("%02x", r->data[k]);
            printf("\n");
        } else if (r->kind == MTK_AWDLCAP_KIND_VIF_RESULT) {
            struct mtk_awdlcap_vifres v;
            memcpy(&v, r->data, sizeof(v));
            printf("VIF step=%u(%s) ret=0x%x role=%u "
                   "mac=%02x:%02x:%02x:%02x:%02x:%02x mtu=%u\n",
                v.step, step_name(v.step), v.ret, v.role,
                v.mac[0], v.mac[1], v.mac[2], v.mac[3], v.mac[4], v.mac[5], v.mtu);
        } else if (r->kind == MTK_AWDLCAP_KIND_SET) {
            printf("SWITCH %u -> %u\n", r->ret, r->enabled);
        } else {
            printf("kind=%u\n", r->kind);
        }
    }
}

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "status";
    struct mtk_awdlcap_state st;
    size_t outsz = sizeof(st);
    kern_return_t kr;

    if (strcmp(cmd, "status") && strcmp(cmd, "on") && strcmp(cmd, "off") &&
        strcmp(cmd, "create") && strcmp(cmd, "drop")) {
        fprintf(stderr, "usage: awdlcap [status|on|off|create|drop]\n");
        return 2;
    }
    io_connect_t c = conn_open();
    if (!c) return 1;
    memset(&st, 0, sizeof(st));
    if (!strcmp(cmd, "status")) {
        kr = IOConnectCallStructMethod(c, kMtkAwdlCapGet, NULL, 0, &st, &outsz);
    } else if (!strcmp(cmd, "on") || !strcmp(cmd, "off")) {
        struct mtk_awdlcap_set req = { .enabled = !strcmp(cmd, "on") };
        kr = IOConnectCallStructMethod(c, kMtkAwdlCapSet, &req, sizeof(req), &st, &outsz);
    } else if (!strcmp(cmd, "create")) {
        kr = IOConnectCallStructMethod(c, kMtkAwdlCapVifMake, NULL, 0, &st, &outsz);
    } else {
        kr = IOConnectCallStructMethod(c, kMtkAwdlCapVifDrop, NULL, 0, &st, &outsz);
    }
    IOServiceClose(c);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "%s: 0x%x%s\n", cmd, kr,
            kr == kIOReturnUnsupported ? " (kext without MTK_AWDL_CAP_PROBE?)" : "");
        return 1;
    }
    if (outsz != sizeof(st) || st.abi != MTK_AWDLCAP_ABI) {
        fprintf(stderr, "unexpected reply size=%zu abi=%u\n", outsz, st.abi);
        return 1;
    }
    print_state(&st);
    return 0;
}
