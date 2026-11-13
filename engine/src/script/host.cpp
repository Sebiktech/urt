#include "host.hpp"

#include "urt/core/log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>

#include <lua.h>
#include <lualib.h>

namespace urt::script {
namespace {

constexpr std::size_t max_handlers_per_resource = 1024;
constexpr std::size_t max_timers_per_resource = 1024;
constexpr std::size_t max_log_message_bytes = 4000;
constexpr double max_timer_seconds = 1.0e9;

// --- urt.* functions both sides have ---------------------------------------------

// urt.log(...) and print(...): one log line, values separated by spaces.
int api_log(lua_State* L) {
    const Resource& resource = resource_of(L);
    std::string text;
    const int count = lua_gettop(L);
    for (int i = 1; i <= count && text.size() < max_log_message_bytes; ++i) {
        std::size_t length = 0;
        const char* part = luaL_tolstring(L, i, &length); // works for any value
        if (i > 1) {
            text += ' ';
        }
        text.append(part, length);
        lua_pop(L, 1);
    }
    if (text.size() > max_log_message_bytes) {
        text.resize(max_log_message_bytes);
    }
    // Scripts on a client come from a server we may not trust: keep them
    // from forging other log lines with line breaks.
    std::replace_if(text.begin(), text.end(), [](char c) { return c == '\n' || c == '\r'; }, ' ');
    log::info("script", "[{}] {}", resource.name, text);
    return 0;
}

void store_handler(lua_State* L, Resource& resource, std::vector<int>& handlers) {
    if (resource.handler_count >= max_handlers_per_resource) {
        luaL_error(L, "too many event handlers");
    }
    // A reference keeps the function alive and lets us find it again later.
    handlers.push_back(lua_ref(L, 2));
    ++resource.handler_count;
}

// urt.on(event, handler): events sent by the engine.
int api_on(lua_State* L) {
    Resource& resource = resource_of(L);
    const char* event = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!resource.host->known_event(event)) {
        luaL_error(L, "unknown event '%s' on the %s (for events sent by scripts, use urt.onNet)", event,
                   resource.host->side());
    }
    store_handler(L, resource, resource.handlers[event]);
    return 0;
}

// urt.onNet(event, handler): events sent by scripts on the other side.
int api_on_net(lua_State* L) {
    Resource& resource = resource_of(L);
    std::size_t length = 0;
    const char* event = luaL_checklstring(L, 1, &length);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!net::valid_event_name(std::string_view(event, length))) {
        luaL_error(L, "an event name is 1 to 64 characters without spaces");
    }
    store_handler(L, resource, resource.net_handlers[std::string(event, length)]);
    return 0;
}

int add_timer(lua_State* L, bool repeating) {
    Resource& resource = resource_of(L);
    const double seconds = check_number(L, 1, max_timer_seconds, "seconds");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (seconds < 0.0) {
        luaL_error(L, "seconds must not be negative");
    }
    if (resource.timers.size() >= max_timers_per_resource) {
        luaL_error(L, "too many timers");
    }
    Timer timer;
    timer.id = resource.next_timer_id++;
    timer.handler = lua_ref(L, 2);
    timer.due = resource.host->time() + seconds;
    timer.interval = repeating ? std::max(seconds, 1.0e-3) : 0.0;
    resource.timers.push_back(timer);
    lua_pushnumber(L, timer.id);
    return 1;
}

// urt.after(seconds, fn) -> timer id
int api_after(lua_State* L) { return add_timer(L, false); }

// urt.every(seconds, fn) -> timer id
int api_every(lua_State* L) { return add_timer(L, true); }

// urt.clearTimer(id) -> true if there was such a timer
int api_clear_timer(lua_State* L) {
    Resource& resource = resource_of(L);
    const double id = luaL_checknumber(L, 1);
    const auto found = std::find_if(resource.timers.begin(), resource.timers.end(),
                                    [&](const Timer& timer) { return timer.id == id; });
    if (found == resource.timers.end()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_unref(L, found->handler);
    resource.timers.erase(found);
    lua_pushboolean(L, 1);
    return 1;
}

// urt.time() -> simulated seconds since this side started
int api_time(lua_State* L) {
    lua_pushnumber(L, resource_of(L).host->time());
    return 1;
}

} // namespace

// --- helpers --------------------------------------------------------------------------

Resource& resource_of(lua_State* L) {
    return *static_cast<Resource*>(lua_touserdata(L, lua_upvalueindex(1)));
}

void add_function(lua_State* L, Resource& resource, const char* name, int (*function)(lua_State*)) {
    // Every function carries a pointer to its resource as an "upvalue", so
    // it knows who is calling without any global lookup.
    lua_pushlightuserdata(L, &resource);
    lua_pushcclosure(L, function, name, 1);
    lua_setfield(L, -2, name);
}

double check_number(lua_State* L, int index, double limit, const char* what) {
    const double value = luaL_checknumber(L, index);
    if (!std::isfinite(value) || std::abs(value) > limit) {
        luaL_error(L, "%s must be a number between %g and %g", what, -limit, limit);
    }
    return value;
}

net::NetId check_entity_id(lua_State* L, int index) {
    const double value = luaL_checknumber(L, index);
    if (!(value >= 1.0 && value <= 4294967295.0) || value != std::floor(value)) {
        luaL_error(L, "entity id must be a positive whole number");
    }
    return static_cast<net::NetId>(value);
}

double optional_field(lua_State* L, int table, const char* field, double limit) {
    lua_getfield(L, table, field);
    double value = 0.0;
    if (!lua_isnil(L, -1)) {
        if (!lua_isnumber(L, -1)) {
            luaL_error(L, "field '%s' must be a number", field);
        }
        value = lua_tonumber(L, -1);
        if (!std::isfinite(value) || std::abs(value) > limit) {
            luaL_error(L, "field '%s' must be between %g and %g", field, -limit, limit);
        }
    }
    lua_pop(L, 1);
    return value;
}

// --- HostBase ---------------------------------------------------------------------------

HostBase::HostBase(HostLimits limits) : limits_(limits) {
    limits_.memory_limit_bytes = std::max<std::size_t>(limits_.memory_limit_bytes, 1024 * 1024);
}

bool HostBase::run_scripts(Resource& resource, const std::vector<net::ScriptFile>& scripts) {
    resource.error.clear();
    try {
        resource.vm = std::make_unique<Vm>(limits_.memory_limit_bytes);
        if (resource.vm->state() == nullptr) {
            fail(resource, "cannot create a script VM");
            return false;
        }

        lua_State* L = resource.vm->state();
        lua_newtable(L);
        add_function(L, resource, "log", api_log);
        add_function(L, resource, "on", api_on);
        add_function(L, resource, "onNet", api_on_net);
        add_function(L, resource, "after", api_after);
        add_function(L, resource, "every", api_every);
        add_function(L, resource, "clearTimer", api_clear_timer);
        add_function(L, resource, "time", api_time);
        lua_pushlstring(L, resource.name.data(), resource.name.size());
        lua_setfield(L, -2, "resourceName");
        register_side_api(resource, L);
        lua_setglobal(L, "urt");

        // print() goes to the log too, instead of a console nobody reads.
        lua_pushlightuserdata(L, &resource);
        lua_pushcclosure(L, api_log, "print", 1);
        lua_setglobal(L, "print");

        resource.vm->seal();
        // Running from here on: the scripts' own top-level code may already
        // register handlers and use the API.
        resource.state = ResourceState::Running;

        for (const net::ScriptFile& script : scripts) {
            std::string error;
            if (!resource.vm->load(script.name, script.source, error)) {
                fail(resource, error);
                return false;
            }
            // Loading may do one-off setup work, so it gets more time than
            // a handler that runs every tick.
            const CallResult result = resource.vm->call(0, limits_.max_call_seconds * 4.0, error);
            if (result != CallResult::Ok) {
                fail(resource, error);
                return false;
            }
        }
    } catch (const std::exception& e) {
        // Luau reports running out of memory outside a script as an exception.
        fail(resource, e.what());
        return false;
    }

    log::info("script", "started {} resource '{}'", side(), resource.name);
    return true;
}

void HostBase::fail(Resource& resource, std::string error) {
    log::error("script", "[{}] stopped: {}", resource.name, error);
    shut_down(resource);
    resource.error = std::move(error);
    on_failed(resource);
}

void HostBase::shut_down(Resource& resource) {
    release_world(resource);
    resource.handlers.clear();
    resource.net_handlers.clear();
    resource.handler_count = 0;
    resource.timers.clear();
    resource.vm.reset(); // closing the VM releases every function reference
    resource.state = ResourceState::Failed;
}

// Calls one script function. Returns false if the resource is no longer
// running afterwards.
bool HostBase::invoke(Resource& resource, int handler, std::string_view what, const PushArguments& push_arguments) {
    std::string error;
    CallResult result = CallResult::Killed;
    try {
        lua_State* L = resource.vm->state();
        lua_getref(L, handler);
        const int arguments = push_arguments ? push_arguments(L) : 0;
        if (arguments < 0) {
            lua_pop(L, 1); // the call was cancelled
            return true;
        }
        result = resource.vm->call(arguments, limits_.max_call_seconds, error);
    } catch (const std::exception& e) {
        error = e.what(); // out of memory while preparing the call
    }

    if (result == CallResult::Killed) {
        fail(resource, std::string(what) + " " + error);
        return false;
    }
    if (result == CallResult::Error) {
        // An ordinary script error: report it and carry on, as a web page
        // carries on after one failed click handler.
        log::error("script", "[{}] error in {}: {}", resource.name, what, error);
    }

    const double seconds = resource.vm->last_call_seconds();
    if (seconds > limits_.slow_call_seconds && Clock::now() - resource.last_slow_warning > std::chrono::seconds(5)) {
        resource.last_slow_warning = Clock::now();
        log::warn("script", "[{}] {} took {:.1f} ms", resource.name, what, seconds * 1000.0);
    }
    return true;
}

void HostBase::call_handlers(Resource& resource, const std::vector<int>& handlers, const std::string& what,
                             const PushArguments& push_arguments) {
    for (const int handler : handlers) {
        if (!invoke(resource, handler, what, push_arguments)) {
            return;
        }
    }
}

void HostBase::dispatch(Resource& resource, const char* event, const PushArguments& push_arguments) {
    if (resource.state != ResourceState::Running) {
        return;
    }
    const auto found = resource.handlers.find(event);
    if (found == resource.handlers.end()) {
        return;
    }
    // A copy: a handler may register more handlers while we go through them.
    const std::vector<int> handlers = found->second;
    call_handlers(resource, handlers, std::string("the '") + event + "' handler", push_arguments);
}

void HostBase::dispatch_net(Resource& resource, const std::string& event, const PushArguments& push_arguments) {
    if (resource.state != ResourceState::Running) {
        return;
    }
    const auto found = resource.net_handlers.find(event);
    if (found == resource.net_handlers.end()) {
        return;
    }
    const std::vector<int> handlers = found->second;
    call_handlers(resource, handlers, "the handler of network event '" + event + "'", push_arguments);
}

void HostBase::run_timers(Resource& resource) {
    if (resource.state != ResourceState::Running || resource.timers.empty()) {
        return;
    }
    // Collect ids first: a timer's function may add or clear timers.
    std::vector<int> due;
    for (const Timer& timer : resource.timers) {
        if (timer.due <= time_) {
            due.push_back(timer.id);
        }
    }
    for (const int id : due) {
        const auto by_id = [id](const Timer& timer) { return timer.id == id; };
        auto timer = std::find_if(resource.timers.begin(), resource.timers.end(), by_id);
        if (timer == resource.timers.end()) {
            continue; // cleared by an earlier timer this tick
        }
        const int handler = timer->handler;
        if (!invoke(resource, handler, "a timer", {})) {
            return;
        }
        timer = std::find_if(resource.timers.begin(), resource.timers.end(), by_id);
        if (timer == resource.timers.end()) {
            continue; // it cleared itself
        }
        if (timer->interval > 0.0) {
            timer->due = time_ + timer->interval;
        } else {
            lua_unref(resource.vm->state(), timer->handler);
            resource.timers.erase(timer);
        }
    }
}

} // namespace urt::script
