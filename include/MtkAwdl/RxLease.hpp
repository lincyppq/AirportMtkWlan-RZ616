/*
 * RxLease.hpp — per-object publication and teardown decisions.
 *
 * The kernel object and the host test both call these functions. They do
 * not allocate, sleep, or touch a work loop.
 *
 * Retain rule, matching 25G241 OSObject: the final release stores a dead
 * sentinel and calls free(). retain() from free() panics. lifeRetainAllowed
 * is false at refcount 0. A deferred stop must retain while the caller
 * still holds a live reference, before it returns. free() runs only after
 * lifeMayDestroy is already true. It does not retain and it does not start
 * another stop.
 *
 * The provider token is stored in the same transition that leaves the
 * object unpublished, before any other thread can observe that state.
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
    uintptr_t providerToken = 0;
};

inline bool lifeRetainAllowed(uint32_t refcount)
{
    return refcount > 0;
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
    life->cleanupClaimed = 0;
    life->providerToken = 0;
    return true;
}

inline LifeAction lifeEnter(Life *life)
{
    if (life == nullptr || life->published == 0 || life->phase != LifePhase::Published)
        return LifeAction::Closed;
    life->inflight++;
    return LifeAction::Entered;
}

/* providerToken is recorded before the phase change becomes the value a
 * later lock acquisition observes. Zero means the caller had no provider. */
inline LifeAction lifeRequestStop(Life *life, bool gateHeld, uintptr_t providerToken)
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
    if (providerToken != 0)
        life->providerToken = providerToken;
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

inline bool lifeClaimCleanup(Life *life)
{
    if (life == nullptr || life->phase != LifePhase::Cleaning || life->cleanupClaimed != 0)
        return false;
    life->cleanupClaimed = 1;
    return true;
}

inline bool lifeOwesCleanupRelease(const Life &life)
{
    return life.cleanupArmed != 0;
}

inline void lifeNoteCleaned(Life *life)
{
    if (life == nullptr || life->phase != LifePhase::Cleaning)
        return;
    life->phase = LifePhase::Dead;
    life->published = 0;
    life->inflight = 0;
    life->cleanupArmed = 0;
    life->cleanupClaimed = 0;
}

inline bool lifeMayDestroy(const Life &life)
{
    if (life.phase == LifePhase::Dead)
        return true;
    return life.phase == LifePhase::Idle && life.cleanupArmed == 0 && life.inflight == 0;
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
