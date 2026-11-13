#pragma once

// Job system: a fixed pool of worker threads that run small tasks.
//
// Creating a thread per task is slow, so the engine creates the threads once
// and hands them work through a queue. Systems use parallel_for to spread a
// loop over all cores inside one tick.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace urt {

class JobSystem {
public:
    using Job = std::function<void()>;
    using RangeBody = std::function<void(std::size_t begin, std::size_t end)>;

    // worker_count 0 means "hardware threads minus one", leaving a core for
    // the thread that calls parallel_for (it does work too). At least 1.
    explicit JobSystem(unsigned worker_count = 0);

    // Finishes every queued job, then joins the workers.
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    unsigned worker_count() const { return static_cast<unsigned>(workers_.size()); }

    // Queues a job. A job that throws is logged and dropped.
    void submit(Job job);

    // Blocks until the queue is empty and no job is running.
    // Do not call from inside a job: it would wait for itself.
    void wait_idle();

    // Calls body(begin, end) for consecutive chunks covering [0, count) and
    // returns when all of them are done. The calling thread takes chunks too,
    // so this is safe to call from inside a job.
    // The body runs on several threads at once: chunks must not write to the
    // same data.
    void parallel_for(std::size_t count, std::size_t chunk_size, const RangeBody& body);

private:
    void worker_loop(unsigned index);

    std::vector<std::thread> workers_;

    std::mutex mutex_;                 // guards everything below
    std::condition_variable work_cv_;  // signalled when a job is queued or on shutdown
    std::condition_variable idle_cv_;  // signalled when the system becomes idle
    std::deque<Job> queue_;
    std::size_t running_ = 0;          // jobs currently executing
    bool stopping_ = false;
};

} // namespace urt
