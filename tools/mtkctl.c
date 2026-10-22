// mtkctl — minimal CLI for SimpleMtkWlan via ClientKit (replaces SimpleMTK.app
// for testing, so no Xcode is needed).
//
//   mtkctl status
//   mtkctl scan
//   mtkctl connect <ssid>      (password read from the terminal, not argv)
//   mtkctl disconnect
//   mtkctl power on|off
//   mtkctl tune [ht=0|1] [ba=0|1] [vht=0|1] [bw=20|40|80] [txba=0|1] [sndq=N] [txs=N] [reassoc=1]
//                                  (apply from the next association; reassoc=1 rejoins now)

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include "Api.h"

static void print_bssid(const uint8_t *b) {
    printf("%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
}

static int cmd_status(void) {
    platform_info_t pi;
    if (!get_platform_info(&pi)) {
        fprintf(stderr, "driver not found (is SimpleMtkWlan.kext loaded?)\n");
        return 1;
    }
    printf("device: %s\ndriver: %s\n", pi.device_info_str, pi.driver_info_str);
    printf("power:  %s\n", is_power_on() ? "on" : "off");
    uint32_t st = 0;
    if (get_80211_state(&st)) printf("state:  %u%s\n", st, st == MTK80211_S_RUN ? " (RUN)" : "");
    station_info_t si = {0};
    if (get_station_info(&si) == KERN_SUCCESS && si.ssid[0]) {
        printf("ssid:   %.32s\nbssid:  ", si.ssid);
        print_bssid(si.bssid);
        printf("\nchan:   %u  bw: %u MHz  rssi: %d  noise: %d  rate: %u  mcs: %d\n",
               si.channel, si.band_width, si.rssi, si.noise, si.rate, si.cur_mcs);
    }
    return 0;
}

static int cmd_scan(void) {
    static network_info_list_t list;
    // get_network_list returns the previous results and triggers a new scan.
    get_network_list(&list);
    sleep(4);
    if (!get_network_list(&list)) {
        fprintf(stderr, "scan failed\n");
        return 1;
    }
    printf("%-32s  %-17s  %4s  %5s\n", "SSID", "BSSID", "CH", "RSSI");
    for (int i = 0; i < list.count; i++) {
        struct ioctl_network_info *n = &list.networks[i];
        printf("%-32.32s  ", n->ssid[0] ? (char *)n->ssid : "<hidden>");
        print_bssid(n->bssid);
        printf("  %4u  %5d\n", n->channel, n->rssi);
    }
    printf("%d networks\n", list.count);
    return 0;
}

static int read_password_file(const char *path, char *buf, size_t capacity) {
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != getuid() || (st.st_mode & 077) != 0) {
        fprintf(stderr, "password file must be a regular file owned by this user with mode 600\n");
        return 0;
    }

    FILE *file = fopen(path, "r");
    if (!file) {
        perror("could not open password file");
        return 0;
    }
    size_t length = fread(buf, 1, capacity - 1, file);
    int extra = fgetc(file);
    int failed = ferror(file) || extra != EOF;
    fclose(file);
    if (failed) {
        fprintf(stderr, "password file is too long or could not be read\n");
        memset(buf, 0, capacity);
        return 0;
    }

    if (length && buf[length - 1] == '\n') length--;
    if (length && buf[length - 1] == '\r') length--;
    for (size_t i = 0; i < length; i++) {
        if (buf[i] == '\r' || buf[i] == '\n') {
            fprintf(stderr, "password file must contain one line\n");
            memset(buf, 0, capacity);
            return 0;
        }
    }
    if (length == 0) {
        fprintf(stderr, "password file is empty\n");
        memset(buf, 0, capacity);
        return 0;
    }
    buf[length] = '\0';
    return 1;
}

static int cmd_connect(const char *ssid, const char *password_file) {
    char ssidbuf[MAX_SSID_LENGTH + 1] = {0};
    char pwdbuf[WPA_KEY_LEN] = {0};
    strncpy(ssidbuf, ssid, MAX_SSID_LENGTH);
    if (password_file) {
        if (!read_password_file(password_file, pwdbuf, sizeof(pwdbuf)))
            return 2;
    } else {
        char *pwd = getpass("password: ");
        strncpy(pwdbuf, pwd ? pwd : "", sizeof(pwdbuf) - 1);
        if (pwd) memset(pwd, 0, strlen(pwd));
    }
    printf("connecting to %s ...\n", ssidbuf);
    bool ok = connect_network(ssidbuf, pwdbuf);
    memset(pwdbuf, 0, sizeof(pwdbuf));
    printf(ok ? "connected\n" : "connect failed or timed out\n");
    return ok ? 0 : 1;
}

static int cmd_disconnect(void) {
    station_info_t si = {0};
    char ssidbuf[MAX_SSID_LENGTH + 1] = {0};
    if (get_station_info(&si) == KERN_SUCCESS) memcpy(ssidbuf, si.ssid, MAX_SSID_LENGTH);
    return dis_associate_ssid(ssidbuf) == KERN_SUCCESS ? 0 : 1;
}

static int cmd_tune(int argc, char **argv) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                   IOServiceMatching("SimpleMtkWlan"));
    if (!svc) {
        fprintf(stderr, "driver not found\n");
        return 1;
    }
    int rc = 0;
    if (argc > 0) {
        CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        for (int i = 0; i < argc; i++) {
            char key[16];
            int val;
            if (sscanf(argv[i], "%15[a-z]=%d", key, &val) != 2 ||
                (strcmp(key, "ht") && strcmp(key, "ba") && strcmp(key, "vht") &&
                 strcmp(key, "bw") && strcmp(key, "txba") && strcmp(key, "sndq") &&
                 strcmp(key, "txs") && strcmp(key, "reassoc"))) {
                fprintf(stderr, "bad argument: %s (use ht=0|1 ba=0|1 vht=0|1 bw=20|40|80 txba=0|1 sndq=N txs=N reassoc=1)\n", argv[i]);
                rc = 2;
                continue;
            }
            CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
            CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &val);
            CFDictionarySetValue(d, k, v);
            CFRelease(k);
            CFRelease(v);
        }
        if (rc == 0) {
            kern_return_t kr = IORegistryEntrySetCFProperty(svc, CFSTR("SMTWTune"), d);
            if (kr != KERN_SUCCESS) {
                fprintf(stderr, "set failed: 0x%x\n", kr);
                rc = 1;
            }
        }
        CFRelease(d);
    }
    CFTypeRef cur = IORegistryEntryCreateCFProperty(svc, CFSTR("SMTWTune"), NULL, 0);
    if (cur && CFGetTypeID(cur) == CFDictionaryGetTypeID()) {
        int ht = -1, ba = -1, vht = -1, bw = -1, txba = -1, sndq = -1, txs = -1;
        CFNumberRef n;
        if ((n = CFDictionaryGetValue(cur, CFSTR("ht")))) CFNumberGetValue(n, kCFNumberIntType, &ht);
        if ((n = CFDictionaryGetValue(cur, CFSTR("ba")))) CFNumberGetValue(n, kCFNumberIntType, &ba);
        if ((n = CFDictionaryGetValue(cur, CFSTR("vht")))) CFNumberGetValue(n, kCFNumberIntType, &vht);
        if ((n = CFDictionaryGetValue(cur, CFSTR("bw")))) CFNumberGetValue(n, kCFNumberIntType, &bw);
        if ((n = CFDictionaryGetValue(cur, CFSTR("txba")))) CFNumberGetValue(n, kCFNumberIntType, &txba);
        if ((n = CFDictionaryGetValue(cur, CFSTR("sndq")))) CFNumberGetValue(n, kCFNumberIntType, &sndq);
        if ((n = CFDictionaryGetValue(cur, CFSTR("txs")))) CFNumberGetValue(n, kCFNumberIntType, &txs);
        printf("tune: ht=%d ba=%d vht=%d bw=%d txba=%d sndq=%d txs=%d\n", ht, ba, vht, bw, txba, sndq, txs);
    } else {
        printf("tune: not supported by this driver build\n");
    }
    if (cur) CFRelease(cur);
    IOObjectRelease(svc);
    return rc;
}

int main(int argc, char **argv) {
    int rc = 2;
    if (argc >= 2 && !strcmp(argv[1], "status")) rc = cmd_status();
    else if (argc >= 2 && !strcmp(argv[1], "scan")) rc = cmd_scan();
    else if (argc == 3 && !strcmp(argv[1], "connect")) rc = cmd_connect(argv[2], NULL);
    else if (argc == 5 && !strcmp(argv[1], "connect") &&
             !strcmp(argv[3], "--password-file"))
        rc = cmd_connect(argv[2], argv[4]);
    else if (argc >= 2 && !strcmp(argv[1], "disconnect")) rc = cmd_disconnect();
    else if (argc >= 3 && !strcmp(argv[1], "power"))
        rc = (!strcmp(argv[2], "on") ? power_on() : power_off()) == KERN_SUCCESS ? 0 : 1;
    else if (argc >= 2 && !strcmp(argv[1], "tune")) rc = cmd_tune(argc - 2, argv + 2);
    else fprintf(stderr, "usage: mtkctl status|scan|connect <ssid> [--password-file <path>]|disconnect|power on|off|tune [ht= ba= vht= bw= txba= sndq= txs= reassoc=1]\n");
    api_terminate();
    return rc;
}
