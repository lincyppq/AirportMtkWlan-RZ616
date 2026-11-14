/* Threads the same lease the kernel receive path uses.
 * One producer is paused after rxLeaseTryEnter and before the work-loop
 * gate, which is the pre-gate window. Stop unpublishes, then waits, and
 * only then marks the object freed. A late producer must not touch it.
 */
#include <MtkAwdl/RxLease.hpp>
#include <atomic>
#include <chrono>
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

struct Object {
    std::atomic<int> freed{0};
    bool draining = false;
    std::atomic<int> touches{0};
};

struct World {
    std::mutex publication;
    std::mutex workloop;
    MtkAwdl::RxLease lease;
    Object *live = nullptr;
    Object storage;
    std::atomic<int> preGate{0};
    std::atomic<int> stopperWaiting{0};
    std::atomic<int> useAfterFree{0};
    std::atomic<int> timedOut{0};
};

static void producer(World *world, bool pauseBeforeGate)
{
    std::unique_lock<std::mutex> lock(world->publication);
    Object *live = world->live;
    if (live == nullptr || !MtkAwdl::rxLeaseTryEnter(&world->lease))
        return;
    lock.unlock();
    if (pauseBeforeGate) {
        world->preGate.store(1, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (world->stopperWaiting.load(std::memory_order_acquire) == 0) {
            if (std::chrono::steady_clock::now() > deadline) {
                world->timedOut.store(1);
                break;
            }
            std::this_thread::yield();
        }
    }
    {
        std::lock_guard<std::mutex> gate(world->workloop);
        if (live->freed.load(std::memory_order_acquire) != 0)
            world->useAfterFree.fetch_add(1);
        else if (live->draining)
            world->useAfterFree.fetch_add(1);
        else
            live->touches.fetch_add(1, std::memory_order_relaxed);
    }
    lock.lock();
    MtkAwdl::rxLeaseLeave(&world->lease);
}

static void stopWhilePreGate(World *world)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (world->preGate.load(std::memory_order_acquire) == 0) {
        if (std::chrono::steady_clock::now() > deadline) {
            world->timedOut.store(1);
            return;
        }
        std::this_thread::yield();
    }
    {
        std::lock_guard<std::mutex> lock(world->publication);
        world->live = nullptr;
        MtkAwdl::rxLeaseUnpublish(&world->lease);
    }
    world->stopperWaiting.store(1, std::memory_order_release);
    MtkAwdl::rxLeaseWait(&world->lease,
        [&world]() { world->publication.lock(); },
        [&world]() { world->publication.unlock(); },
        []() { std::this_thread::yield(); });
    world->storage.draining = true;
    world->storage.freed.store(1, std::memory_order_release);
}

static void testPreGate()
{
    World world;
    world.live = &world.storage;
    MtkAwdl::rxLeasePublish(&world.lease);
    std::thread rx(producer, &world, true);
    std::thread halt(stopWhilePreGate, &world);
    rx.join();
    halt.join();
    producer(&world, false);
    expect(world.timedOut.load() == 0, "pre-gate stop finished");
    expect(world.useAfterFree.load() == 0, "pre-gate producer saw a live object");
    expect(world.storage.touches.load() == 1, "pre-gate producer entered the gate once");
    expect(world.storage.freed.load() == 1, "object freed after the lease");
    expect(MtkAwdl::rxLeaseInflight(&world.lease) == 0, "lease quiescent");
    expect(world.lease.published == 0, "publication disabled");
}

static void hammer(World *world, std::atomic<int> *run)
{
    while (run->load(std::memory_order_acquire) != 0)
        producer(world, false);
}

static void testHammer()
{
    World world;
    world.live = &world.storage;
    MtkAwdl::rxLeasePublish(&world.lease);
    std::atomic<int> run{1};
    std::thread workers[4];
    for (auto &worker : workers)
        worker = std::thread(hammer, &world, &run);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        std::lock_guard<std::mutex> lock(world.publication);
        world.live = nullptr;
        MtkAwdl::rxLeaseUnpublish(&world.lease);
    }
    MtkAwdl::rxLeaseWait(&world.lease,
        [&world]() { world.publication.lock(); },
        [&world]() { world.publication.unlock(); },
        []() { std::this_thread::yield(); });
    world.storage.draining = true;
    world.storage.freed.store(1, std::memory_order_release);
    run.store(0, std::memory_order_release);
    for (auto &worker : workers)
        worker.join();
    expect(world.useAfterFree.load() == 0, "hammer saw no freed object");
    expect(MtkAwdl::rxLeaseInflight(&world.lease) == 0, "hammer lease quiescent");
    expect(world.storage.touches.load() > 0, "hammer delivered before stop");
}

int main()
{
    testPreGate();
    testHammer();
    if (failures != 0) {
        fprintf(stderr, "%d rx lease failures\n", failures);
        return 1;
    }
    printf("awdl rx lease: ok\n");
    return 0;
}
