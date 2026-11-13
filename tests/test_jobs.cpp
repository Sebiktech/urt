#include <doctest/doctest.h>

#include "urt/core/jobs.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <vector>

using urt::JobSystem;

TEST_CASE("JobSystem runs every submitted job") {
    JobSystem jobs(4);
    CHECK(jobs.worker_count() == 4);

    std::atomic<int> counter{0};
    for (int i = 0; i < 1000; ++i) {
        jobs.submit([&counter] { counter.fetch_add(1); });
    }
    jobs.wait_idle();
    CHECK(counter.load() == 1000);
}

TEST_CASE("JobSystem finishes queued jobs before it is destroyed") {
    std::atomic<int> counter{0};
    {
        JobSystem jobs(2);
        for (int i = 0; i < 200; ++i) {
            jobs.submit([&counter] { counter.fetch_add(1); });
        }
    }
    CHECK(counter.load() == 200);
}

TEST_CASE("JobSystem survives a job that throws") {
    JobSystem jobs(2);
    std::atomic<int> counter{0};
    jobs.submit([] { throw std::runtime_error("expected in this test"); });
    jobs.submit([&counter] { counter.fetch_add(1); });
    jobs.wait_idle();
    CHECK(counter.load() == 1);
}

TEST_CASE("parallel_for visits every index exactly once") {
    JobSystem jobs(4);

    for (const std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{1000},
                                    std::size_t{100'003}}) {
        for (const std::size_t chunk : {std::size_t{0}, std::size_t{1}, std::size_t{64}, std::size_t{5000}}) {
            std::vector<int> visits(count, 0);
            jobs.parallel_for(count, chunk, [&](std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i) {
                    ++visits[i]; // each index belongs to one chunk, so no race
                }
            });
            const bool all_once = std::all_of(visits.begin(), visits.end(), [](int v) { return v == 1; });
            CHECK_MESSAGE(all_once, "count=", count, " chunk=", chunk);
        }
    }
}

TEST_CASE("parallel_for can be nested inside jobs without deadlocking") {
    JobSystem jobs(2);
    std::atomic<std::size_t> total{0};

    // More outer chunks than workers, each starting its own parallel_for.
    jobs.parallel_for(16, 1, [&](std::size_t, std::size_t) {
        jobs.parallel_for(1000, 10, [&](std::size_t begin, std::size_t end) {
            total.fetch_add(end - begin);
        });
    });
    CHECK(total.load() == 16'000);
}

TEST_CASE("parallel_for passes on an exception from the body") {
    JobSystem jobs(4);
    std::atomic<int> started{0};

    CHECK_THROWS_AS(jobs.parallel_for(10'000, 1,
                                      [&](std::size_t begin, std::size_t) {
                                          started.fetch_add(1);
                                          if (begin == 3) {
                                              throw std::runtime_error("expected in this test");
                                          }
                                      }),
                    std::runtime_error);

    // The system is still usable afterwards.
    std::atomic<int> counter{0};
    jobs.parallel_for(100, 1, [&](std::size_t, std::size_t) { counter.fetch_add(1); });
    CHECK(counter.load() == 100);
}
