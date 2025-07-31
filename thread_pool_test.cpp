#include "sp_thread_pool.hpp"
#include <gtest/gtest.h>
#include <vector>
#include <thread>
#include <atomic>
#include <set>
#include <chrono>
#include <iostream>

// Unit tests for the header-only SP::ThreadPool implementation
// Covers initialization, configuration, task submission, splitting, concurrency, and shutdown behaviors.

// 1. Lazy initialization & singleton behavior
TEST(ThreadPoolTest, LazyInitializationAndSingleton) {
    // Ensure shutdown first to reset any existing instance
    SP::ThreadPool::shutdown();

    // No direct access to internal instance, but get_thread_count() should initialize configured_threads
    size_t count1 = SP::ThreadPool::get_thread_count();
    ASSERT_GE(count1, 1u) << "Thread count should be at least 1 after lazy init";

    // Subsequent calls should return the same count
    size_t count2 = SP::ThreadPool::get_thread_count();
    EXPECT_EQ(count1, count2) << "Configured thread count should remain constant unless changed";
}

// 2. Thread-count configuration
TEST(ThreadPoolTest, ThreadCountConfiguration) {
    SP::ThreadPool::shutdown();
    SP::ThreadPool::set_thread_count(4);
    EXPECT_EQ(SP::ThreadPool::get_thread_count(), 4u) << "Should report 4 after setting thread count";

    // Submissions should use 4 threads internally (indirectly test via stress test later)
}

// 3. Basic loop submission
TEST(ThreadPoolTest, BasicLoopSubmission) {
    const size_t N = 96;
    std::vector<int> data(N, 0);
    std::cout << "BasicLoopSubmission" << std::endl;

    auto fut = SP::ThreadPool::submit_task(0, N, [&](size_t i) {
        data[i] = static_cast<int>(i) + 1;
    });
    fut.get();  // wait for completion

    // Verify each slot was written exactly once
    for (size_t i = 0; i < N; ++i) {
        EXPECT_EQ(data[i], static_cast<int>(i) + 1);
    }
}

// 4. Empty-range behavior
TEST(ThreadPoolTest, EmptyRange) {
    std::atomic<bool> called(false);
    auto fut = SP::ThreadPool::submit_task(10, 5, [&](size_t) {
        called = true;
    });
    // Future should be ready immediately
    EXPECT_NO_THROW(fut.get());
    EXPECT_FALSE(called.load()) << "Callback should not be invoked for empty range";
}

// 6. Concurrency & queueing
TEST(ThreadPoolTest, MultipleSubmissionsFromDifferentThreads) {
    std::cout << "Test 6" << std::endl;
    SP::ThreadPool::shutdown();
    std::cout << "Shut down" << std::endl;
    SP::ThreadPool::set_thread_count(2);
    std::cout << "set_thread_count()" << std::endl;
    const size_t N = 50;
    std::vector<int> a(N, 0), b(N, 0);

    // Launch two host threads that each submit a task
    std::thread t1([&](){
        auto f1 = SP::ThreadPool::submit_task(0, N, [&](size_t i) { a[i] = 1; });
        f1.get();
    });
    std::thread t2([&](){
        auto f2 = SP::ThreadPool::submit_task(0, N, [&](size_t i) { b[i] = 2; });
        f2.get();
    });
    t1.join();
    t2.join();

    // Verify each vector was filled correctly
    for (size_t i = 0; i < N; ++i) {
        EXPECT_EQ(a[i], 1);
        EXPECT_EQ(b[i], 2);
    }
}

// 7. Shutdown & re-initialization
TEST(ThreadPoolTest, ShutdownAndReinitialize) {
    SP::ThreadPool::shutdown();
    SP::ThreadPool::set_thread_count(3);
    EXPECT_EQ(SP::ThreadPool::get_thread_count(), 3u);

    // After shutdown, pool should recreate on next submit
    SP::ThreadPool::shutdown();
    auto fut = SP::ThreadPool::submit_task(0, 5, [&](size_t i) {
        // trivial
    });
    EXPECT_NO_THROW(fut.get());
    EXPECT_EQ(SP::ThreadPool::get_thread_count(), 3u);
}

// 8. Stress/concurrency validation
TEST(ThreadPoolTest, StressConcurrencyValidation) {
    SP::ThreadPool::shutdown();
    SP::ThreadPool::set_thread_count(4);
    const size_t N = 1000;
    std::mutex mtx;
    std::set<std::thread::id> thread_ids;

    auto fut = SP::ThreadPool::submit_task(0, N, [&](size_t) {
        std::lock_guard<std::mutex> lg(mtx);
        thread_ids.insert(std::this_thread::get_id());
        // Simulate some work
        std::this_thread::sleep_for(std::chrono::microseconds(1));
    });
    fut.get();

    EXPECT_GT(thread_ids.size(), 1u) << "Should use multiple worker threads";
    EXPECT_LE(thread_ids.size(), 4u) << "Should not exceed configured thread count";
}

// 9. Edge-case resilience: rapid reconfiguration
TEST(ThreadPoolTest, RapidReconfiguration) {
    SP::ThreadPool::shutdown();
    // Rapidly change thread counts when idle
    for (size_t i = 1; i <= 8; ++i) {
        SP::ThreadPool::set_thread_count(i);
        EXPECT_EQ(SP::ThreadPool::get_thread_count(), i);
    }

    // Immediately submit after last resize
    const size_t N = 20;
    std::vector<int> data(N, 0);
    auto fut = SP::ThreadPool::submit_task(0, N, [&](size_t i) { data[i] = 1; });
    fut.get();
    for (auto v : data) EXPECT_EQ(v, 1);
}

// 10. submit_task with explicit chunk count
TEST(ThreadPoolTest, SubmitWithChunkCount) {
    SP::ThreadPool::shutdown();
    SP::ThreadPool::set_thread_count(3);
    const size_t N = 10;
    std::vector<int> data(N, 0);

    // use exactly 3 chunks
    auto fut = SP::ThreadPool::submit_task(0, N, /*chunk_count=*/3, [&](size_t i) {
        data[i] = static_cast<int>(i) + 1;
    });
    fut.get();

    for (size_t i = 0; i < N; ++i) {
        EXPECT_EQ(data[i], static_cast<int>(i) + 1);
    }
}

// 11. submit_task_with_chunk_size
TEST(ThreadPoolTest, SubmitByChunkSize) {
    SP::ThreadPool::shutdown();
    SP::ThreadPool::set_thread_count(4);
    const size_t N = 20;
    std::vector<int> data(N, 0);

    // chunk_size = 5 → 4 chunks
    auto fut = SP::ThreadPool::submit_task_with_chunk_size(0, N, /*chunk_size=*/5, [&](size_t i) {
        data[i] = 1;
    });
    fut.get();

    for (auto v : data) {
        EXPECT_EQ(v, 1);
    }
}

// 12. submit_task_with_threads_cap override
TEST(ThreadPoolTest, ThreadsCapOverride) {
    SP::ThreadPool::shutdown();
    SP::ThreadPool::set_thread_count(4);
    SP::ThreadPool::set_processor_affinity();
    const size_t N = 100;
    std::mutex mtx;
    std::set<std::thread::id> thread_ids;

    // force all work onto a single worker
    auto fut = SP::ThreadPool::submit_task_with_threads_cap(0, N, /*threads_cap=*/1, [&](size_t) {
        std::lock_guard<std::mutex> lg(mtx);
        thread_ids.insert(std::this_thread::get_id());
    });
    fut.get();

    EXPECT_EQ(thread_ids.size(), 1u);
}

// 13. threads_cap override resets after completion
TEST(ThreadPoolTest, ThreadsCapOverrideReset) {
    SP::ThreadPool::shutdown();
    SP::ThreadPool::set_thread_count(4);

    // first with cap=1
    {
        const size_t N = 50;
        std::mutex mtx;
        std::set<std::thread::id> ids1;
        auto f1 = SP::ThreadPool::submit_task_with_threads_cap(0, N, 1, [&](size_t) {
            std::lock_guard<std::mutex> lg(mtx);
            ids1.insert(std::this_thread::get_id());
        });
        f1.get();
        ASSERT_EQ(ids1.size(), 1u);
    }

    // then default (should use >1 thread)
    {
        const size_t N = 50;
        std::mutex mtx;
        std::set<std::thread::id> ids2;
        auto f2 = SP::ThreadPool::submit_task(0, N, [&](size_t) {
            std::lock_guard<std::mutex> lg(mtx);
            ids2.insert(std::this_thread::get_id());
        });
        f2.get();
        EXPECT_GT(ids2.size(), 1u);
        EXPECT_LE(ids2.size(), 4u);
    }
}

// 14. submit_task_with_threads_cap + chunk_multiplier
TEST(ThreadPoolTest, SubmitWithThreadsCapAndChunkMultiplier) {
    SP::ThreadPool::shutdown();
    SP::ThreadPool::set_thread_count(3);
    const size_t N = 30;
    std::vector<int> data(N, 0);

    // threads_cap=2, chunks_multiplier=2
    auto fut = SP::ThreadPool::submit_task_with_threads_cap(0, N, /*threads_cap=*/2,
                                                           /*chunks_multiplier=*/2,
                                                           [&](size_t i) {
        data[i] = 7;
    });
    fut.get();

    for (auto v : data) {
        EXPECT_EQ(v, 7);
    }
}

// 15. soft_boot behavior
TEST(ThreadPoolTest, SoftBoot) {
    SP::ThreadPool::shutdown();
    // configure to 5 threads, but do not initialize yet
    SP::ThreadPool::set_thread_count(5);
    SP::ThreadPool::shutdown();       // tear down again
    SP::ThreadPool::soft_boot();      // should re-create with 5
    EXPECT_EQ(SP::ThreadPool::get_thread_count(), 5u);

    // and basic submit still works
    const size_t N = 8;
    std::vector<int> data(N, 0);
    auto fut = SP::ThreadPool::submit_task(0, N, [&](size_t i) { data[i] = 3; });
    fut.get();
    for (auto v : data) EXPECT_EQ(v, 3);
}

// 16. set_processor_affinity is no-op (should not crash)
TEST(ThreadPoolTest, ProcessorAffinityNoCrash) {
    SP::ThreadPool::shutdown();
    SP::ThreadPool::soft_boot();
    EXPECT_NO_THROW(SP::ThreadPool::set_processor_affinity());
}

// 17. empty-range for chunk & cap variants
TEST(ThreadPoolTest, EmptyRangeAllVariants) {
    // chunk-count variant
    {
        std::atomic<bool> called(false);
        auto f = SP::ThreadPool::submit_task(5, 2, 3, [&](size_t){ called = true; });
        EXPECT_NO_THROW(f.get());
        EXPECT_FALSE(called.load());
    }
    // chunk-size variant
    {
        std::atomic<bool> called(false);
        auto f = SP::ThreadPool::submit_task_with_chunk_size(5, 2, 4, [&](size_t){ called = true; });
        EXPECT_NO_THROW(f.get());
        EXPECT_FALSE(called.load());
    }
    // threads_cap + default-chunk
    {
        std::atomic<bool> called(false);
        auto f = SP::ThreadPool::submit_task_with_threads_cap(5, 2, 2, [&](size_t){ called = true; });
        EXPECT_NO_THROW(f.get());
        EXPECT_FALSE(called.load());
    }
    // threads_cap + multiplier
    {
        std::atomic<bool> called(false);
        auto f = SP::ThreadPool::submit_task_with_threads_cap(5, 2, 2, 3, [&](size_t){ called = true; });
        EXPECT_NO_THROW(f.get());
        EXPECT_FALSE(called.load());
    }
}

// 18. Single-function submission & future readiness
TEST(ThreadPoolTest, SimpleFunctionSubmissionAndFuture) {
    SP::ThreadPool::shutdown();

    std::atomic<bool> called(false);
    auto fut = SP::ThreadPool::submit_task([&](){
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        called = true;
    });

    // Should not be set until we wait on the future
    EXPECT_FALSE(called.load());

    // get() blocks until the lambda runs and sets `called`
    fut.get();
    EXPECT_TRUE(called.load());
}

// 19. Multiple-function submissions + wait_for_all
TEST(ThreadPoolTest, MultipleFunctionSubmissionsAndWaitForAll) {
    SP::ThreadPool::shutdown();
    SP::ThreadPool::set_thread_count(3);

    std::atomic<int> counter{0};
    const int COUNT = 100;

    // fire off COUNT independent tasks
    for (int i = 0; i < COUNT; ++i) {
        SP::ThreadPool::submit_task([&](){
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // We didn't keep the futures, but wait_for_all() should block until all are done
    SP::ThreadPool::wait_for_all();
    EXPECT_EQ(counter.load(), COUNT);
}

// 20. wait_for_all with no pending tasks
TEST(ThreadPoolTest, WaitForAllNoPending) {
    SP::ThreadPool::shutdown();
    // No tasks ever submitted – should return immediately (no deadlock)
    EXPECT_NO_THROW(SP::ThreadPool::wait_for_all());
}

// 21. Mixed use of future-get and wait_for_all
TEST(ThreadPoolTest, FutureGetAndWaitForAllMix) {
    SP::ThreadPool::shutdown();
    SP::ThreadPool::set_thread_count(2);

    std::atomic<int> x{0}, y{0};

    // Submit two tasks, keep one future and discard the other
    auto f1 = SP::ThreadPool::submit_task([&](){ x = 42; });
    SP::ThreadPool::submit_task([&](){ y = 99; });

    // Wait on the first explicitly...
    f1.get();
    EXPECT_EQ(x, 42);
    // ...then use wait_for_all to catch the second
    SP::ThreadPool::wait_for_all();
    EXPECT_EQ(y, 99);
}


int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}