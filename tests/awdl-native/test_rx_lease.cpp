/* Drives MtkAwdl::Life, the same state machine as stopOwning / free /
 * rxLeaseFinished. Retain counts follow lifeCleanupRetainDelta and
 * lifeMayDestroy. A deferred stop must not destroy the object, and the
 * last leave must be what releases the cleanup retain.
 */
#include <MtkAwdl/RxLease.hpp>
#include <atomic>
#include <mutex>
#include <thread>
#include <stdio.h>

static int failures = 0;

static void expect(bool ok, const char *what)
{
    if (ok)
        return;
    fprintf(stderr, "FAIL %s\n", what);
    ++failures;
}

struct Obj {
    MtkAwdl::Life life;
    int retains = 1;
    bool resources = true;
    bool destroyed = false;
    bool gate = false;
    bool scheduled = false;
    int cleans = 0;
    bool providerHeld = false;

    void retain()
    {
        ++retains;
    }

    void release()
    {
        expect(retains > 0, "release below zero");
        if (retains <= 0)
            return;
        --retains;
        if (retains == 0)
            onFree();
    }

    void finish(bool dropCleanupRetain)
    {
        if (!MtkAwdl::lifeClaimCleanup(&life))
            return;
        resources = false;
        ++cleans;
        providerHeld = false;
        MtkAwdl::lifeNoteCleaned(&life);
        if (dropCleanupRetain)
            release();
    }

    MtkAwdl::LifeAction stopOwning(bool onGate)
    {
        const uint32_t armed = life.cleanupArmed;
        const MtkAwdl::LifeAction action = MtkAwdl::lifeRequestStop(&life, onGate);
        if (MtkAwdl::lifeCleanupRetainDelta(action) != 0 && armed == 0)
            retain();
        if (action == MtkAwdl::LifeAction::Defer) {
            providerHeld = true;
            return action;
        }
        if (action == MtkAwdl::LifeAction::Wait) {
            if (MtkAwdl::lifeWaitSatisfied(life))
                finish(false);
            return action;
        }
        if (action == MtkAwdl::LifeAction::Clean)
            finish(false);
        return action;
    }

    void onFree()
    {
        if (!MtkAwdl::lifeMayDestroy(life)) {
            const MtkAwdl::LifeAction action = stopOwning(gate);
            if (action == MtkAwdl::LifeAction::Clean || action == MtkAwdl::LifeAction::Wait) {
                destroyed = true;
                return;
            }
            if (action == MtkAwdl::LifeAction::Already)
                retain();
            return;
        }
        destroyed = true;
    }

    void leave()
    {
        const MtkAwdl::LifeAction action = MtkAwdl::lifeLeave(&life);
        if (action != MtkAwdl::LifeAction::Clean)
            return;
        if (gate) {
            scheduled = true;
            return;
        }
        finish(true);
    }

    void runScheduled()
    {
        expect(!gate, "thread call runs off the gate");
        if (!scheduled)
            return;
        scheduled = false;
        finish(true);
    }
};

static void testRegistrationUnwind()
{
    Obj obj;
    expect(obj.stopOwning(false) == MtkAwdl::LifeAction::Clean, "idle stop cleans");
    expect(!obj.resources && obj.cleans == 1, "registration failure released queues");
    expect(obj.retains == 1 && !obj.destroyed, "creator still owns the object");
    obj.release();
    expect(obj.destroyed && obj.retains == 0, "one final free");
    expect(obj.stopOwning(false) == MtkAwdl::LifeAction::Already, "second stop does nothing");
    expect(obj.cleans == 1, "second stop does not clean again");
}

static void testDeferThenLastLeave()
{
    Obj obj;
    MtkAwdl::lifePublish(&obj.life);
    expect(MtkAwdl::lifeEnter(&obj.life) == MtkAwdl::LifeAction::Entered, "producer enters");
    obj.gate = true;
    expect(obj.stopOwning(true) == MtkAwdl::LifeAction::Defer, "gate-held stop defers");
    expect(obj.retains == 2 && obj.providerHeld, "cleanup retain and provider retain");
    expect(obj.resources && !obj.destroyed, "queues stay until the producer leaves");
    expect(obj.stopOwning(true) == MtkAwdl::LifeAction::Already, "retry does not take another retain");
    expect(obj.retains == 2, "retry retain count");
    obj.release();
    expect(obj.retains == 1 && !obj.destroyed, "creator handoff is not the final free");
    obj.leave();
    expect(obj.scheduled && obj.resources && !obj.destroyed, "cleanup is queued, not run on the gate");
    obj.gate = false;
    obj.runScheduled();
    expect(!obj.resources && obj.destroyed && obj.cleans == 1, "off-gate cleanup is the final free");
    expect(!obj.providerHeld, "provider released after cleanup");
}

static void testFreeWhileDeferred()
{
    Obj obj;
    MtkAwdl::lifePublish(&obj.life);
    expect(MtkAwdl::lifeEnter(&obj.life) == MtkAwdl::LifeAction::Entered, "free-path producer");
    obj.gate = true;
    obj.release();
    expect(!obj.destroyed && obj.retains == 1, "free resurrects instead of destroying");
    expect(obj.life.phase == MtkAwdl::LifePhase::Deferred, "free armed the deferred stop");
    obj.leave();
    obj.gate = false;
    obj.runScheduled();
    expect(obj.destroyed && !obj.resources, "last leave completes the resurrected object");
}

static void testOffGateWait()
{
    Obj obj;
    MtkAwdl::lifePublish(&obj.life);
    expect(MtkAwdl::lifeEnter(&obj.life) == MtkAwdl::LifeAction::Entered, "off-gate producer");
    expect(obj.stopOwning(false) == MtkAwdl::LifeAction::Wait, "off-gate stop waits");
    expect(obj.resources && obj.retains == 1, "no cleanup retain while waiting");
    expect(MtkAwdl::lifeLeave(&obj.life) == MtkAwdl::LifeAction::None, "waiter owns cleanup");
    expect(MtkAwdl::lifeWaitSatisfied(obj.life), "lease is zero");
    obj.finish(false);
    obj.release();
    expect(obj.destroyed && obj.cleans == 1, "waiter cleanup then creator free");
}

struct Shared {
    std::mutex mu;
    Obj obj;
    std::atomic<int> preGate{0};
    std::atomic<int> stopDone{0};
    std::atomic<int> sawLive{0};
};

static void producer(Shared *shared)
{
    std::unique_lock<std::mutex> lock(shared->mu);
    expect(MtkAwdl::lifeEnter(&shared->obj.life) == MtkAwdl::LifeAction::Entered, "thread enter");
    lock.unlock();
    shared->preGate.store(1);
    while (shared->stopDone.load() == 0)
        std::this_thread::yield();
    lock.lock();
    shared->obj.gate = true;
    shared->obj.leave();
    shared->obj.gate = false;
    if (shared->obj.resources)
        shared->sawLive.store(1);
    lock.unlock();
    shared->obj.runScheduled();
}

static void testThreadedPreGate()
{
    Shared shared;
    MtkAwdl::lifePublish(&shared.obj.life);
    std::thread rx(producer, &shared);
    while (shared.preGate.load() == 0)
        std::this_thread::yield();
    MtkAwdl::LifeAction action;
    {
        std::lock_guard<std::mutex> lock(shared.mu);
        shared.obj.gate = true;
        action = shared.obj.stopOwning(true);
        shared.obj.release();
        shared.stopDone.store(1);
    }
    rx.join();
    expect(action == MtkAwdl::LifeAction::Defer, "threaded stop defers");
    expect(shared.sawLive.load() == 1, "producer still saw resources");
    expect(shared.obj.destroyed && !shared.obj.resources, "join sees the final free");
    expect(shared.obj.cleans == 1, "one cleanup");
}

int main()
{
    testRegistrationUnwind();
    testDeferThenLastLeave();
    testFreeWhileDeferred();
    testOffGateWait();
    testThreadedPreGate();
    if (failures != 0) {
        fprintf(stderr, "%d rx lease failures\n", failures);
        return 1;
    }
    printf("awdl rx lease: ok\n");
    return 0;
}
