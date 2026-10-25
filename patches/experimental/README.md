# Experimental patches (not applied, not tested)

Patches in this directory are **not** part of the default build. They have been
compiled but have **not** been installed or run on hardware. Apply them only if you
can recover from a kext that fails to load (keep a bootable EFI backup).

## m3a6-tx-gate-1600.patch (driver tag `rz616-m3a-6`)

Base: this branch (`rz616-m3i-2`). Apply from the repository root:

```sh
git apply patches/experimental/m3a6-tx-gate-1600.patch
./build.sh
```

Changes (5 files):

1. **TX ring serialisation.** `mwx_awdlraw_inject()` ran on the user-client thread
   without the command gate, while `mwx_start`/`mwx_tx` (command gate) and
   `mwx_dma_tx_cleanup` (interrupt, same work loop) update the same TX ring.
   A TX-stall snapshot from the in-driver log showed
   `prod=121 cons=122 cpu=121 dma=122 free=0`, i.e. the consumer one slot ahead of the
   producer, which is consistent with a lost producer update. After such an event the
   ring reports no free slots, raw TX returns `ENOBUFS`, and the m3a-5 watchdog resets
   the chip after 8 s. The patch makes `mwx_awdlraw_api_tx()` enter the command gate
   before injecting. The watchdog is unchanged.
2. **`MTK_AWDLRAW_MAX_FRAME` 800 → 1600.** The ABI is length-driven; user-space tools
   built for 800 keep working.
3. **New user-client selector 8 (`GetInfo2`)**: `max_frame`, driver tag, TX-stall
   counter and a snapshot of the last stall. Older kexts return an error for it.

Status: compiled on macOS 26.7.1; the kext's imported kernel symbols were checked
against the running kernel collection. Not installed, not runtime-tested. Whether it
removes the TX stalls seen during large Mac → iPhone transfers is unknown.
