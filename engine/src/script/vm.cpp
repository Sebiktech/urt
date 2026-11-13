#include "vm.hpp"

#include <chrono>
#include <cstdlib>

#include <lua.h>
#include <luacode.h>
#include <lualib.h>

namespace urt::script {

Vm::Vm(std::size_t memory_limit_bytes) : memory_limit_(memory_limit_bytes) {
    state_ = lua_newstate(&Vm::allocate, this);
    if (state_ == nullptr) {
        return; // the limit is too small even for an empty VM
    }
    lua_callbacks(state_)->userdata = this;
    lua_callbacks(state_)->interrupt = &Vm::interrupt;
    luaL_openlibs(state_);
}

Vm::~Vm() {
    if (state_ != nullptr) {
        lua_close(state_);
    }
}

// Luau's allocator callback: new_size 0 frees, otherwise (re)allocates.
void* Vm::allocate(void* self, void* pointer, std::size_t old_size, std::size_t new_size) {
    Vm& vm = *static_cast<Vm*>(self);
    if (new_size == 0) {
        std::free(pointer);
        vm.memory_used_ -= old_size;
        return nullptr;
    }
    if (new_size > old_size && vm.memory_used_ + (new_size - old_size) > vm.memory_limit_) {
        // Refusing makes Luau raise "not enough memory" in the script. The
        // flag tells call() that this VM is done for.
        vm.out_of_memory_ = true;
        return nullptr;
    }
    void* const result = std::realloc(pointer, new_size);
    if (result != nullptr) {
        vm.memory_used_ = vm.memory_used_ - old_size + new_size;
    }
    return result;
}

// Called by Luau at every loop iteration and function call (gc < 0), and
// during garbage collection (gc >= 0), where we must not interfere.
void Vm::interrupt(lua_State* state, int gc) {
    if (gc >= 0) {
        return;
    }
    Vm& vm = *static_cast<Vm*>(lua_callbacks(state)->userdata);
    if (vm.out_of_memory_) {
        // The script already hit its memory limit. It may be catching the
        // resulting errors in a loop; do not let it run to the deadline.
        lua_break(state);
        return;
    }
    if (!vm.has_deadline_ || Clock::now() < vm.deadline_) {
        return;
    }
    vm.timed_out_ = true;
    // lua_break suspends the coroutine as a debugger breakpoint would. The
    // script cannot catch this with pcall, and we never resume it. (Inside a
    // C function such as table.sort Luau cannot suspend and raises an error
    // instead; the interrupt then fires again at the next loop iteration,
    // one level further out, until the code is suspended or has unwound.)
    lua_break(state);
}

void Vm::seal() {
    luaL_sandbox(state_);

    // The standard library is now read-only and shared. Scripts need a
    // global table they can write to: a thread gets its own, which falls
    // back to the shared one for everything it does not define itself.
    environment_ = lua_newthread(state_);
    environment_ref_ = lua_ref(state_, -1);
    lua_pop(state_, 1);
    luaL_sandboxthread(environment_);
}

bool Vm::load(const std::string& chunk_name, std::string_view source, std::string& error) {
    // Luau never runs source text directly: it is compiled to bytecode here
    // and the VM loads that. There is no way for a script to do the same.
    std::size_t bytecode_size = 0;
    char* const bytecode = luau_compile(source.data(), source.size(), nullptr, &bytecode_size);
    if (bytecode == nullptr) {
        error = "out of memory while compiling";
        return false;
    }
    // A leading '=' makes Luau use the name as is in error messages.
    const std::string name = "=" + chunk_name;
    // Loaded on the environment thread so the function's globals are the
    // script environment; a syntax error comes back as a load failure.
    lua_State* const target = environment_ != nullptr ? environment_ : state_;
    const int status = luau_load(target, name.c_str(), bytecode, bytecode_size, 0);
    std::free(bytecode);

    if (status != 0) {
        std::size_t length = 0;
        const char* message = lua_tolstring(target, -1, &length);
        error = message != nullptr ? std::string(message, length) : "unknown error";
        lua_pop(target, 1);
        return false;
    }
    if (target != state_) {
        lua_xmove(target, state_, 1);
    }
    return true;
}

CallResult Vm::call(int nargs, double max_seconds, std::string& error,
                    const std::function<void(lua_State* results, int count)>& on_results) {
    // A fresh coroutine per call: only a coroutine can be suspended by the
    // interrupt, and a script error cannot leave anything on the main stack.
    lua_State* const thread = lua_newthread(state_);
    lua_insert(state_, -(nargs + 2));      // move the thread below function and arguments
    lua_xmove(state_, thread, nargs + 1);  // hand function and arguments to it

    timed_out_ = false;
    has_deadline_ = max_seconds > 0.0;
    const Clock::time_point start = Clock::now();
    if (has_deadline_) {
        deadline_ = start + std::chrono::duration_cast<Nanoseconds>(std::chrono::duration<double>(max_seconds));
    }

    const int status = lua_resume(thread, nullptr, nargs);

    has_deadline_ = false;
    last_call_seconds_ = std::chrono::duration<double>(Clock::now() - start).count();

    CallResult result = CallResult::Ok;
    if (timed_out_) {
        error = "ran for too long and was stopped";
        result = CallResult::Killed;
    } else if (out_of_memory_) {
        error = "used too much memory and was stopped";
        result = CallResult::Killed;
    } else if (status == LUA_OK || status == LUA_YIELD) {
        // A top-level yield has nothing to wait for; treat it as a return.
        if (on_results) {
            on_results(thread, lua_gettop(thread));
        }
    } else {
        std::size_t length = 0;
        const char* message = lua_tolstring(thread, -1, &length);
        error = message != nullptr ? std::string(message, length) : "error value is not a string";
        result = CallResult::Error;
    }

    lua_pop(state_, 1); // the thread; the garbage collector frees it
    return result;
}

} // namespace urt::script
