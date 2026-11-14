/*
 * RxLease.hpp — publication lease for the software receive path.
 *
 * A producer may use the interface only after rxLeaseTryEnter, and only
 * until rxLeaseLeave. Both run while the caller holds the publication lock.
 * The producer drops that lock before IOWorkLoop::runAction and takes it
 * again only to leave. Teardown calls rxLeaseUnpublish under the same lock
 * before it waits, and it does not hold the lock or the work-loop gate
 * across rxLeaseWait. The wait covers a producer that has already entered
 * and has not reached runAction yet.
 *
 * An atomic load of the object pointer followed by a retain is not this
 * protocol. The lease count is the ownership. The object and its work loop
 * stay allocated until the count returns to zero.
 *
 * GPLv2.
 */
#pragma once
#include <stdint.h>

namespace MtkAwdl {

struct RxLease {
    uint32_t published = 0;
    uint32_t inflight = 0;
};

inline bool rxLeaseTryEnter(RxLease *lease)
{
    if (lease == nullptr || lease->published == 0)
        return false;
    lease->inflight++;
    return true;
}

inline void rxLeaseLeave(RxLease *lease)
{
    if (lease != nullptr && lease->inflight > 0)
        lease->inflight--;
}

inline void rxLeasePublish(RxLease *lease)
{
    if (lease != nullptr)
        lease->published = 1;
}

inline void rxLeaseUnpublish(RxLease *lease)
{
    if (lease != nullptr)
        lease->published = 0;
}

inline uint32_t rxLeaseInflight(const RxLease *lease)
{
    return lease == nullptr ? 0u : lease->inflight;
}

/* Lock and unlock bracket only the inflight read. pause runs with the
 * publication lock released, so a producer can leave and a work-loop
 * action can run. */
template <typename Lock, typename Unlock, typename Pause>
inline void rxLeaseWait(RxLease *lease, Lock lock, Unlock unlock, Pause pause)
{
    for (;;) {
        lock();
        const uint32_t inflight = rxLeaseInflight(lease);
        unlock();
        if (inflight == 0)
            return;
        pause();
    }
}

} /* namespace MtkAwdl */
