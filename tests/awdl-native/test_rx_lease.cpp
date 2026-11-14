/* Per-object Life, with the 25G241 rule that retain at refcount 0 is
 * forbidden. Final release before lifeMayDestroy fails the test. Provider
 * token is part of the stop transition. Each object has its own Life.
 */
#include <MtkAwdl/RxLease.hpp>
#include <mutex>
#include <thread>
#include <stdio.h>

static int failures = 0;
static int violations = 0;

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
    int cleans = 0;
    bool detached = false;
    Obj **slot = nullptr;

    void retain()
    {
        if (!MtkAwdl::lifeRetainAllowed(static_cast<uint32_t>(retains))) {
            ++violations;
            return;
        }
        ++retains;
    }

    void release()
    {
        if (retains <= 0) {
            expect(false, "release at refcount 0");
            return;
        }
        if (retains == 1 && !MtkAwdl::lifeMayDestroy(life)) {
            expect(false, "final release before cleanup");
            return;
        }
        --retains;
        if (retains == 0) {
            expect(MtkAwdl::lifeMayDestroy(life), "free only when destroy is legal");
            destroyed = true;
        }
    }

    void finish()
    {
        if (!MtkAwdl::lifeClaimCleanup(&life))
            return;
        const bool drop = MtkAwdl::lifeOwesCleanupRelease(life);
        const uintptr_t token = life.providerToken;
        resources = false;
        ++cleans;
        if (drop && token != 0)
            detached = true;
        MtkAwdl::lifeNoteCleaned(&life);
        if (slot != nullptr && *slot == this)
            *slot = nullptr;
        if (drop)
            release();
    }

    MtkAwdl::LifeAction stopOwning(bool onGate, uintptr_t provider)
    {
        const bool starting = life.phase == MtkAwdl::LifePhase::Idle ||
            life.phase == MtkAwdl::LifePhase::Published;
        if (starting && provider != 0)
            life.providerToken = provider;
        const uint32_t armed = life.cleanupArmed;
        const MtkAwdl::LifeAction action = MtkAwdl::lifeRequestStop(&life, onGate,
            life.providerToken);
        if (MtkAwdl::lifeCleanupRetainDelta(action) != 0 && armed == 0)
            retain();
        if (action == MtkAwdl::LifeAction::Defer)
            return action;
        if (action == MtkAwdl::LifeAction::Wait) {
            if (MtkAwdl::lifeWaitSatisfied(life))
                finish();
            return action;
        }
        if (action == MtkAwdl::LifeAction::Clean)
            finish();
        return action;
    }
};

static void testRetainAtZeroForbidden()
{
    expect(!MtkAwdl::lifeRetainAllowed(0), "kernel retain at 0 is rejected");
    expect(MtkAwdl::lifeRetainAllowed(1), "live retain is allowed");
    Obj obj;
    obj.retains = 0;
    obj.retain();
    expect(violations == 1 && obj.retains == 0 && !obj.destroyed, "harness does not resurrect");
    violations = 0;
}

static void testRegistrationUnwind()
{
    Obj obj;
    expect(obj.stopOwning(false, 0x11) == MtkAwdl::LifeAction::Clean, "idle stop cleans");
    expect(!obj.resources && obj.cleans == 1 && !obj.detached, "no extra cleanup retain");
    expect(obj.life.providerToken == 0x11, "provider recorded with the stop");
    obj.release();
    expect(obj.destroyed, "creator release is the free");
    expect(obj.stopOwning(false, 0x11) == MtkAwdl::LifeAction::Already, "repeat stop");
    expect(obj.cleans == 1, "cleanup once");
}

static void testDeferAndOnGateLeave()
{
    Obj obj;
    MtkAwdl::lifePublish(&obj.life);
    expect(MtkAwdl::lifeEnter(&obj.life) == MtkAwdl::LifeAction::Entered, "enter");
    expect(obj.stopOwning(true, 0x22) == MtkAwdl::LifeAction::Defer, "defer");
    expect(obj.retains == 2 && obj.life.providerToken == 0x22, "cleanup retain and provider");
    expect(obj.stopOwning(true, 0x99) == MtkAwdl::LifeAction::Already, "retry keeps one retain");
    expect(obj.retains == 2 && obj.life.providerToken == 0x22, "provider not replaced");
    obj.release();
    expect(obj.retains == 1 && !obj.destroyed, "creator handoff");
    expect(MtkAwdl::lifeLeave(&obj.life) == MtkAwdl::LifeAction::Clean, "last leave");
    expect(obj.life.providerToken == 0x22, "leave observes the published provider");
    obj.finish();
    expect(obj.destroyed && obj.detached && obj.cleans == 1, "one deferred cleanup");
    obj.finish();
    expect(obj.cleans == 1, "second claim does nothing");
}

static void testDeferredRecoveryStop()
{
    Obj obj;
    MtkAwdl::lifePublish(&obj.life);
    expect(MtkAwdl::lifeEnter(&obj.life) == MtkAwdl::LifeAction::Entered, "recovery enter");
    expect(obj.stopOwning(true, 0x33) == MtkAwdl::LifeAction::Defer, "recovery defer");
    obj.release();
    expect(MtkAwdl::lifeLeave(&obj.life) == MtkAwdl::LifeAction::Clean, "leave grants clean");
    /* The leave has not finished. An off-gate stop must not add a retain,
     * and only one of the two completions releases. */
    expect(obj.stopOwning(false, 0x44) == MtkAwdl::LifeAction::Already, "claim belongs to the leave");
    obj.finish();
    expect(obj.retains == 0 && obj.destroyed && obj.cleans == 1, "single cleanup release");
}

static void testOffGateWait()
{
    Obj obj;
    MtkAwdl::lifePublish(&obj.life);
    expect(MtkAwdl::lifeEnter(&obj.life) == MtkAwdl::LifeAction::Entered, "wait enter");
    expect(obj.stopOwning(false, 0x55) == MtkAwdl::LifeAction::Wait, "off-gate wait");
    expect(obj.retains == 1, "wait does not take a cleanup retain");
    expect(MtkAwdl::lifeLeave(&obj.life) == MtkAwdl::LifeAction::None, "waiter owns cleanup");
    obj.finish();
    obj.release();
    expect(obj.destroyed && obj.cleans == 1 && !obj.detached, "creator free after wait");
}

enum class Stage : int {
    Unreserved = 0,
    Reserved = 1,
    WorkloopRejected = 2,
    LockFailed = 3,
    StartFailed = 4,
    PoolsFailed = 5,
    QueuesFailed = 6,
    RegisterFailed = 7
};

struct World {
    Obj *live = nullptr;
};

static void reserve(World &world, Obj &obj)
{
    expect(world.live == nullptr, "slot empty before reserve");
    world.live = &obj;
    obj.slot = &world.live;
}

static void abandon(World &world, Obj &obj)
{
    const bool ok = MtkAwdl::lifeAbandonReservation(&obj.life);
    expect(ok, "unpublished reservation can be abandoned");
    if (ok && world.live == &obj)
        world.live = nullptr;
}

static void testBringupFailure(Stage stage)
{
    World world;
    Obj keeper;
    reserve(world, keeper);
    Obj failed;
    if (stage == Stage::Unreserved) {
        failed.release();
        expect(failed.destroyed && failed.life.phase == MtkAwdl::LifePhase::Idle,
            "unreserved init failure stays off the slot");
        expect(world.live == &keeper && keeper.life.phase == MtkAwdl::LifePhase::Idle,
            "unreserved failure does not withdraw another object");
        return;
    }
    abandon(world, keeper);
    keeper.release();
    expect(world.live == nullptr, "keeper withdrawn");
    reserve(world, failed);
    if (stage >= Stage::PoolsFailed) {
        expect(failed.stopOwning(false, 0x70 + static_cast<uintptr_t>(stage)) ==
            MtkAwdl::LifeAction::Clean, "started failure stops");
        expect(world.live == nullptr && failed.life.phase == MtkAwdl::LifePhase::Dead,
            "stop withdraws this reservation");
        failed.release();
    } else {
        abandon(world, failed);
        expect(failed.life.phase == MtkAwdl::LifePhase::Dead, "early failure is Dead");
        expect(world.live == nullptr, "early failure withdraws gLive");
        failed.retains = 2;
        failed.release();
        expect(!failed.destroyed && world.live == nullptr, "extra retain is not a Busy wedge");
        failed.release();
    }
    expect(failed.destroyed, "failed object reaches free");
    Obj next;
    reserve(world, next);
    expect(world.live == &next && MtkAwdl::lifePublish(&next.life), "next generation can publish");
    expect(failed.life.phase == MtkAwdl::LifePhase::Dead, "old life stays Dead");
    abandon(world, failed);
    expect(world.live == &next && next.life.phase == MtkAwdl::LifePhase::Published,
        "old abandon does not withdraw the new object");
}

static void testBringupStages()
{
    const Stage stages[] = {
        Stage::Unreserved, Stage::Reserved, Stage::WorkloopRejected, Stage::LockFailed,
        Stage::StartFailed, Stage::PoolsFailed, Stage::QueuesFailed, Stage::RegisterFailed
    };
    for (Stage stage : stages)
        testBringupFailure(stage);
}

struct Shared {
    std::mutex mu;
    Obj *live = nullptr;
};

static void testGenerationAndProvider()
{
    Shared shared;
    Obj oldObj;
    Obj newObj;
    oldObj.slot = &shared.live;
    newObj.slot = &shared.live;
    {
        std::lock_guard<std::mutex> lock(shared.mu);
        MtkAwdl::lifePublish(&oldObj.life);
        shared.live = &oldObj;
        expect(MtkAwdl::lifeEnter(&oldObj.life) == MtkAwdl::LifeAction::Entered, "old enter");
        expect(oldObj.stopOwning(true, 0x66) == MtkAwdl::LifeAction::Defer, "old defer");
    }
    oldObj.release();
    std::thread leaver([&shared, &oldObj]() {
        std::lock_guard<std::mutex> lock(shared.mu);
        expect(MtkAwdl::lifeLeave(&oldObj.life) == MtkAwdl::LifeAction::Clean, "old leave");
        expect(oldObj.life.providerToken == 0x66, "token visible at leave");
        oldObj.finish();
    });
    leaver.join();
    expect(oldObj.destroyed && shared.live == nullptr, "old free cleared the slot");
    {
        std::lock_guard<std::mutex> lock(shared.mu);
        shared.live = &newObj;
        expect(MtkAwdl::lifePublish(&newObj.life), "new generation publishes");
    }
    expect(newObj.life.phase == MtkAwdl::LifePhase::Published, "new life is not the old one");
    expect(!newObj.destroyed && newObj.retains == 1, "new object was not freed");
    expect(oldObj.life.phase == MtkAwdl::LifePhase::Dead, "old life stays dead");
}

int main()
{
    testRetainAtZeroForbidden();
    testRegistrationUnwind();
    testDeferAndOnGateLeave();
    testDeferredRecoveryStop();
    testOffGateWait();
    testGenerationAndProvider();
    testBringupStages();
    expect(violations == 0, "no scenario retained at refcount 0");
    if (failures != 0) {
        fprintf(stderr, "%d rx lease failures\n", failures);
        return 1;
    }
    printf("awdl rx lease: ok\n");
    return 0;
}
