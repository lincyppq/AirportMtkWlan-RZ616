/*
 * RxLease.hpp — shared publication and teardown state machine.
 *
 * The kernel and the host test both call these functions. They do not
 * allocate, sleep, or touch a work loop. The caller supplies whether the
 * station work loop gate is already held.
 *
 * Retain rule:
 *   lifeRequestStop returns Defer only when this thread holds the work-loop
 *   gate and a producer has already entered. The caller then adds exactly
 *   one retain (lifeCleanupRetainDelta). That retain, not a raw pointer,
 *   keeps the object alive. The matching release happens inside the cleanup
 *   that runs after the last lifeLeave, and only after resources are gone.
 *   lifeMayDestroy is true only once that cleanup has recorded Dead.
 *   free() must not destroy while lifeMayDestroy is false; it keeps the
 *   retain instead.
 *
 * Schedule rule:
 *   Cleanup that sees the gate held is not run on that thread. The last
 *   lifeLeave returns Clean at most once. If that thread is on the gate,
 *   the caller queues a thread call. The call is not the controller work
 *   loop. If the thread is already off the gate, the caller cleans inline.
 *   The object's own work-loop retain, taken when the queues were created,
 *   is what cleanup uses. A provider passed into a deferred stop is
 *   retained for super::stop and released after that call returns.
 *
 * GPLv2.
 */
#pragma once
#include <stdint.h>

namespace MtkAwdl {

enum class LifePhase : uint32_t {
    Idle = 0,
    Published = 1,
    Deferred = 2,
    Cleaning = 3,
    Dead = 4
};

enum class LifeAction : uint32_t {
    None = 0,
    Entered = 1,
    Closed = 2,
    Wait = 3,
    Defer = 4,
    Clean = 5,
    Already = 6
};

struct Life {
    LifePhase phase = LifePhase::Idle;
    uint32_t inflight = 0;
    uint32_t published = 0;
    uint32_t cleanupArmed = 0;
    uint32_t cleanupClaimed = 0;
};

inline void lifePrepareNew(Life *life)
{
    if (life == nullptr)
        return;
    if (life->phase == LifePhase::Idle || life->phase == LifePhase::Dead) {
        life->phase = LifePhase::Idle;
        life->published = 0;
        life->inflight = 0;
        life->cleanupArmed = 0;
        life->cleanupClaimed = 0;
    }
}

inline bool lifePublish(Life *life)
{
    if (life == nullptr)
        return false;
    if (life->phase != LifePhase::Idle && life->phase != LifePhase::Dead)
        return false;
    life->phase = LifePhase::Published;
    life->published = 1;
    life->inflight = 0;
    life->cleanupArmed = 0;
    return true;
}

inline LifeAction lifeEnter(Life *life)
{
    if (life == nullptr || life->published == 0 || life->phase != LifePhase::Published)
        return LifeAction::Closed;
    life->inflight++;
    return LifeAction::Entered;
}

/* gateHeld is true only when this thread already owns the station work
 * loop gate. Waiting in that state would deadlock a producer blocked in
 * runAction, so the action is Defer instead of Wait. */
inline LifeAction lifeRequestStop(Life *life, bool gateHeld)
{
    if (life == nullptr)
        return LifeAction::Already;
    if (life->phase == LifePhase::Deferred) {
        if (!gateHeld && life->inflight == 0) {
            life->phase = LifePhase::Cleaning;
            return LifeAction::Clean;
        }
        return LifeAction::Already;
    }
    if (life->phase == LifePhase::Cleaning || life->phase == LifePhase::Dead)
        return LifeAction::Already;
    life->published = 0;
    if (gateHeld && life->inflight > 0) {
        life->phase = LifePhase::Deferred;
        life->cleanupArmed = 1;
        return LifeAction::Defer;
    }
    if (life->inflight > 0) {
        life->phase = LifePhase::Cleaning;
        return LifeAction::Wait;
    }
    life->phase = LifePhase::Cleaning;
    return LifeAction::Clean;
}

/* The last leave while Deferred is the only leave that returns Clean.
 * The caller must not run that cleanup while it still holds the work-loop
 * gate. Queue it instead. */
inline LifeAction lifeLeave(Life *life)
{
    if (life == nullptr || life->inflight == 0)
        return LifeAction::None;
    life->inflight--;
    if (life->inflight == 0 && life->phase == LifePhase::Deferred) {
        life->phase = LifePhase::Cleaning;
        return LifeAction::Clean;
    }
    return LifeAction::None;
}

inline void lifeNoteCleaned(Life *life)
{
    if (life == nullptr)
        return;
    if (life->phase == LifePhase::Cleaning)
        life->phase = LifePhase::Dead;
    life->published = 0;
    life->inflight = 0;
    life->cleanupArmed = 0;
    life->cleanupClaimed = 0;
}

inline bool lifeClaimCleanup(Life *life)
{
    if (life == nullptr || life->phase != LifePhase::Cleaning || life->cleanupClaimed != 0)
        return false;
    life->cleanupClaimed = 1;
    return true;
}

inline bool lifeMayDestroy(const Life &life)
{
    return life.phase == LifePhase::Dead;
}

inline int lifeCleanupRetainDelta(LifeAction action)
{
    return action == LifeAction::Defer ? 1 : 0;
}

inline bool lifeWaitSatisfied(const Life &life)
{
    return life.inflight == 0;
}

} /* namespace MtkAwdl */
