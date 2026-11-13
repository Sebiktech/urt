#pragma once

// One Luau virtual machine with limits on memory and running time.
//
// Internal to the scripting module: the only header that exposes Luau types.
//
// Scripts are not trusted. Three things keep one from harming the server:
//   - Luau's standard library has no file, process or network access, and
//     no way to load code from strings or bytecode.
//   - Every allocation goes through our allocator, which refuses to grow
//     past the memory limit.
//   - Every call has a deadline. Luau calls our interrupt function at each
//     loop iteration and function call; past the deadline it suspends the
//     running code in a way the script cannot catch or undo.

#include "urt/core/time.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

struct lua_State;

namespace urt::script {

enum class CallResult {
    Ok,
    Error,  // the script raised an error; it can keep running afterwards
    Killed, // the script hit a limit; the VM must not be used again
};

class Vm {
public:
    explicit Vm(std::size_t memory_limit_bytes);
    ~Vm();

    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;

    // The main state. Global functions registered on it before seal() become
    // part of the read-only environment every script sees.
    lua_State* state() { return state_; }

    // Finishes setup: makes the standard library and everything registered so
    // far read-only, and creates the writable global table scripts run in.
    void seal();

    // Compiles `source` and pushes the resulting function onto the main
    // state's stack. On failure pushes nothing and fills `error`.
    bool load(const std::string& chunk_name, std::string_view source, std::string& error);

    // Calls a function with its arguments already on the main state's stack:
    // push the function first, then its `nargs` arguments. Both are removed.
    // The function runs in a fresh coroutine for at most max_seconds
    // (0 = no limit). `on_results`, if given, can read the returned values
    // from the coroutine's stack before they are discarded.
    CallResult call(int nargs, double max_seconds, std::string& error,
                    const std::function<void(lua_State* results, int count)>& on_results = {});

    // Seconds the last call() took.
    double last_call_seconds() const { return last_call_seconds_; }

    std::size_t memory_used() const { return memory_used_; }

private:
    static void* allocate(void* self, void* pointer, std::size_t old_size, std::size_t new_size);
    static void interrupt(lua_State* state, int gc);

    lua_State* state_ = nullptr;
    int environment_ref_ = 0; // keeps the script environment's thread alive
    lua_State* environment_ = nullptr;

    std::size_t memory_limit_;
    std::size_t memory_used_ = 0;
    bool out_of_memory_ = false;

    bool has_deadline_ = false;
    bool timed_out_ = false;
    Clock::time_point deadline_{};
    double last_call_seconds_ = 0.0;
};

} // namespace urt::script
