#include "urt/core/jobs.hpp"

#include "urt/core/log.hpp"
#include "urt/core/platform.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <memory>
#include <string>

namespace urt {

JobSystem::JobSystem(unsigned worker_count) {
    if (worker_count == 0) {
        const unsigned hardware = platform::hardware_threads();
        worker_count = hardware > 1 ? hardware - 1 : 1;
    }
    workers_.reserve(worker_count);
    for (unsigned i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this, i] { worker_loop(i); });
    }
}

JobSystem::~JobSystem() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    work_cv_.notify_all();
    for (std::thread& worker : workers_) {
        worker.join();
    }
}

void JobSystem::submit(Job job) {
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(job));
    }
    work_cv_.notify_one();
}

void JobSystem::wait_idle() {
    std::unique_lock lock(mutex_);
    idle_cv_.wait(lock, [this] { return queue_.empty() && running_ == 0; });
}

void JobSystem::worker_loop(unsigned index) {
    const std::string name = "urt-job-" + std::to_string(index);
    platform::set_current_thread_name(name.c_str());

    std::unique_lock lock(mutex_);
    for (;;) {
        // Sleep until there is work. On shutdown, drain the queue first.
        work_cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty()) {
            return; // only reachable when stopping_
        }

        Job job = std::move(queue_.front());
        queue_.pop_front();
        ++running_;

        // Run the job without holding the lock so other workers can take jobs.
        lock.unlock();
        try {
            job();
        } catch (const std::exception& e) {
            log::error("jobs", "job threw an exception: {}", e.what());
        } catch (...) {
            log::error("jobs", "job threw an unknown exception");
        }
        job = nullptr; // destroy captured state outside the lock as well
        lock.lock();

        --running_;
        if (queue_.empty() && running_ == 0) {
            idle_cv_.notify_all();
        }
    }
}

namespace {

// Shared between the caller of parallel_for and its helper jobs.
struct ParallelState {
    std::atomic<std::size_t> next{0}; // first index nobody has taken yet
    std::size_t count = 0;
    std::size_t chunk_size = 1;
    const JobSystem::RangeBody* body = nullptr;

    std::mutex mutex;
    std::condition_variable done_cv;
    std::size_t helpers_running = 0;
    bool closed = false;              // set by the caller once all chunks are taken
    std::exception_ptr helper_error;  // first exception thrown by a helper
};

// Takes chunks until none are left. Each fetch_add hands out a unique range,
// so no two threads ever process the same index.
void run_chunks(ParallelState& state) {
    for (;;) {
        const std::size_t begin = state.next.fetch_add(state.chunk_size);
        if (begin >= state.count) {
            return;
        }
        const std::size_t end = std::min(begin + state.chunk_size, state.count);
        (*state.body)(begin, end);
    }
}

// Makes the remaining chunks unavailable. Used when a body throws, so the
// other threads stop as soon as they finish their current chunk.
void cancel_remaining(ParallelState& state) { state.next.store(state.count); }

} // namespace

void JobSystem::parallel_for(std::size_t count, std::size_t chunk_size, const RangeBody& body) {
    if (count == 0) {
        return;
    }
    chunk_size = std::max<std::size_t>(chunk_size, 1);

    const std::size_t chunk_count = (count + chunk_size - 1) / chunk_size;
    const std::size_t helper_count = std::min<std::size_t>(workers_.size(), chunk_count - 1);

    // One chunk, or nobody to share with: just run it here.
    if (helper_count == 0) {
        body(0, count);
        return;
    }

    auto state = std::make_shared<ParallelState>();
    state->count = count;
    state->chunk_size = chunk_size;
    state->body = &body;

    for (std::size_t i = 0; i < helper_count; ++i) {
        submit([state] {
            {
                std::lock_guard lock(state->mutex);
                if (state->closed) {
                    return; // the caller already finished everything
                }
                ++state->helpers_running;
            }

            std::exception_ptr error;
            try {
                run_chunks(*state);
            } catch (...) {
                error = std::current_exception();
                cancel_remaining(*state);
            }

            std::lock_guard lock(state->mutex);
            if (error && !state->helper_error) {
                state->helper_error = error;
            }
            --state->helpers_running;
            state->done_cv.notify_all();
        });
    }

    // The caller works too instead of just waiting.
    std::exception_ptr caller_error;
    try {
        run_chunks(*state);
    } catch (...) {
        caller_error = std::current_exception();
        cancel_remaining(*state);
    }

    // All chunks are taken now. Helpers that have not started yet are turned
    // away by `closed`; we only wait for the ones in the middle of a chunk.
    // Waiting for unstarted helpers instead could deadlock when every worker
    // is itself inside a parallel_for.
    std::exception_ptr helper_error;
    {
        std::unique_lock lock(state->mutex);
        state->closed = true;
        state->done_cv.wait(lock, [&] { return state->helpers_running == 0; });
        helper_error = state->helper_error;
    }

    // `body` is no longer used by anyone, so it is safe to leave this scope.
    if (caller_error) {
        std::rethrow_exception(caller_error);
    }
    if (helper_error) {
        std::rethrow_exception(helper_error);
    }
}

} // namespace urt
