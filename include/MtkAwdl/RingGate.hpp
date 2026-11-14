/*
 * RingGate.hpp — when a station-ring mutator may run.
 * GPLv2. See docs/AWDL-NATIVE.md.
 *
 * The production serializer is the HAL command gate on the same workloop as
 * mwx_intr. This predicate is the reset rule that production and the host
 * test share. It does not model DMA, firmware, or IOCommandGate.
 */
#ifndef MtkAwdl_RingGate_hpp
#define MtkAwdl_RingGate_hpp

namespace MtkAwdl {

/* Reset and queue teardown change every index and the TXWI pool. They run
 * only when this caller can take the gate, nobody else holds it, and detach
 * is not waiting on that gate. */
inline bool ringResetMayRun(bool gatePresent, bool heldByOther, bool detaching)
{
    return gatePresent && !heldByOther && !detaching;
}

}

#endif
