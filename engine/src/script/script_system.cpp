#include "urt/script/script_system.hpp"

#include "vm.hpp"

#include "urt/core/log.hpp"
#include "urt/ecs/components.hpp"
#include "urt/net/server.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <functional>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <lua.h>
#include <lualib.h>

namespace urt::script {
namespace {

constexpr const char* manifest_file = "resource.lua";
constexpr std::size_t max_script_bytes = 1024 * 1024;
constexpr std::size_t max_scripts_per_resource = 256;
constexpr std::size_t max_handlers_per_resource = 1024;
constexpr std::size_t max_timers_per_resource = 1024;
constexpr std::size_t max_log_message_bytes = 4000;
constexpr double max_coordinate = 1.0e6; // metres from the origin
constexpr double max_speed = 1000.0;     // metres per second
constexpr double max_timer_seconds = 1.0e9;
constexpr std::size_t manifest_memory_bytes = 4u * 1024u * 1024u;

// Events the engine sends to scripts.
constexpr const char* known_events[] = {"playerJoined", "playerLeft", "tick", "stop"};

class Host;

struct Timer {
    int id = 0;
    int handler = 0;       // registry reference to the function
    double due = 0.0;      // server time at which it fires next
    double interval = 0.0; // 0 = fires once
};

struct Resource {
    Host* host = nullptr;
    std::string name;
    std::filesystem::path directory;
    ResourceState state = ResourceState::Failed;
    std::string error;

    std::unique_ptr<Vm> vm;
    std::unordered_map<std::string, std::vector<int>> handlers; // event name -> function references
    std::size_t handler_count = 0;
    std::vector<Timer> timers;
    int next_timer_id = 1;
    std::unordered_set<net::NetId> entities; // spawned by this resource, removed with it

    // Files whose change triggers a reload, with the time we last saw.
    std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> watched;
    Clock::time_point last_slow_warning{};
};

struct Manifest {
    std::vector<std::string> server_scripts;
};

// Resource names become part of log lines and, later, of network messages.
bool valid_resource_name(std::string_view name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

// A script path from a manifest must stay inside the resource's folder.
bool valid_script_path(std::string_view path) {
    if (path.empty() || path.size() > 200 || path.front() == '/' || path.find("..") != std::string_view::npos) {
        return false;
    }
    const bool safe_characters = std::all_of(path.begin(), path.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               c == '.' || c == '/';
    });
    return safe_characters && (path.ends_with(".lua") || path.ends_with(".luau"));
}

bool read_file(const std::filesystem::path& path, std::string& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path.filename().string();
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (out.size() > max_script_bytes) {
        error = path.filename().string() + " is larger than 1 MB";
        return false;
    }
    return true;
}

std::filesystem::file_time_type modified_time(const std::filesystem::path& path) {
    std::error_code ignored;
    return std::filesystem::last_write_time(path, ignored); // the minimum time if the file is gone
}

// --- helpers for functions called from Lua -------------------------------------

Resource& resource_of(lua_State* L) {
    return *static_cast<Resource*>(lua_touserdata(L, lua_upvalueindex(1)));
}

// Reads a number argument and rejects NaN, infinity and absurd sizes. One bad
// number in a position would be sent to every client nearby.
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

// Reads an optional numeric field such as { x = 3 }. Missing means 0.
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

void push_player(lua_State* L, net::NetId id, const std::string& name, const glm::vec3& position) {
    lua_createtable(L, 0, 5);
    lua_pushnumber(L, id);
    lua_setfield(L, -2, "id");
    lua_pushlstring(L, name.data(), name.size());
    lua_setfield(L, -2, "name");
    lua_pushnumber(L, position.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, position.y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, position.z);
    lua_setfield(L, -2, "z");
}

// --- the host: everything behind ScriptSystem ------------------------------------

class Host {
public:
    Host(net::Server& server, ScriptConfig config) : server_(server), config_(std::move(config)) {
        config_.memory_limit_bytes = std::max<std::size_t>(config_.memory_limit_bytes, 1024 * 1024);
    }

    void tick(World& world, const TickContext& context);
    bool load(World& world, const std::filesystem::path& directory);
    bool stop(World& world, std::string_view name);
    std::vector<ResourceInfo> resources() const;

    // Used by the functions scripts call. Only valid while a script runs.
    net::Server& server() { return server_; }
    World& world() { return *world_; }
    double time() const { return time_; }
    const ScriptConfig& config() const { return config_; }

private:
    Resource* find(std::string_view name);
    bool load_directory(const std::filesystem::path& directory);
    bool start(Resource& resource);
    bool read_manifest(Resource& resource, Manifest& manifest);
    void register_api(Resource& resource);
    void fail(Resource& resource, std::string error);
    void shut_down(Resource& resource);
    void restart(Resource& resource);

    bool invoke(Resource& resource, int handler, std::string_view what,
                const std::function<int(lua_State*)>& push_arguments);
    void dispatch(Resource& resource, const char* event, const std::function<int(lua_State*)>& push_arguments = {});
    void run_timers(Resource& resource);
    void check_files();

    net::Server& server_;
    ScriptConfig config_;
    // unique_ptr: scripts hold pointers to their Resource, so it must not move.
    std::vector<std::unique_ptr<Resource>> resources_;
    World* world_ = nullptr;
    double time_ = 0.0;
    bool started_ = false;
    Clock::time_point last_file_check_{};
};

// --- the urt.* functions -----------------------------------------------------------

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
    log::info("script", "[{}] {}", resource.name, text);
    return 0;
}

// urt.on(event, handler)
int api_on(lua_State* L) {
    Resource& resource = resource_of(L);
    const char* event = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    const bool known = std::any_of(std::begin(known_events), std::end(known_events),
                                   [&](const char* name) { return std::string_view(name) == event; });
    if (!known) {
        luaL_error(L, "unknown event '%s' (events are playerJoined, playerLeft, tick, stop)", event);
    }
    if (resource.handler_count >= max_handlers_per_resource) {
        luaL_error(L, "too many event handlers");
    }
    // A reference keeps the function alive and lets us find it again later.
    resource.handlers[event].push_back(lua_ref(L, 2));
    ++resource.handler_count;
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

// urt.spawn({ x, y, z, yaw }) -> entity id. Every field is optional.
int api_spawn(lua_State* L) {
    Resource& resource = resource_of(L);
    Host& host = *resource.host;

    Transform transform;
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TTABLE);
        transform.position = {static_cast<float>(optional_field(L, 1, "x", max_coordinate)),
                              static_cast<float>(optional_field(L, 1, "y", max_coordinate)),
                              static_cast<float>(optional_field(L, 1, "z", max_coordinate))};
        transform.yaw = static_cast<float>(optional_field(L, 1, "yaw", max_coordinate));
    }

    if (resource.entities.size() >= host.config().max_entities_per_resource) {
        // Forget entities that no longer exist before giving up.
        std::erase_if(resource.entities,
                      [&](net::NetId id) { return host.server().find(host.world(), id) == entt::null; });
        if (resource.entities.size() >= host.config().max_entities_per_resource) {
            luaL_error(L, "this resource already has %d entities, the most allowed",
                       static_cast<int>(resource.entities.size()));
        }
    }

    const net::NetId id = host.server().spawn(host.world(), transform);
    resource.entities.insert(id);
    lua_pushnumber(L, id);
    return 1;
}

// urt.despawn(id) -> true if the entity existed
int api_despawn(lua_State* L) {
    Resource& resource = resource_of(L);
    const net::NetId id = check_entity_id(L, 1);
    if (!resource.entities.contains(id)) {
        luaL_error(L, "entity %d was not spawned by this resource", static_cast<int>(id));
    }
    resource.entities.erase(id);
    lua_pushboolean(L, resource.host->server().despawn(resource.host->world(), id) ? 1 : 0);
    return 1;
}

// urt.exists(id) -> boolean
int api_exists(lua_State* L) {
    Host& host = *resource_of(L).host;
    lua_pushboolean(L, host.server().find(host.world(), check_entity_id(L, 1)) != entt::null ? 1 : 0);
    return 1;
}

// Finds the entity for the id in argument 1, or raises a script error.
Entity check_entity(lua_State* L) {
    Host& host = *resource_of(L).host;
    const net::NetId id = check_entity_id(L, 1);
    const Entity entity = host.server().find(host.world(), id);
    if (entity == entt::null) {
        luaL_error(L, "there is no entity with id %d", static_cast<int>(id));
    }
    return entity;
}

// urt.getPosition(id) -> x, y, z
int api_get_position(lua_State* L) {
    const Entity entity = check_entity(L);
    const Transform& transform = resource_of(L).host->world().registry.get<Transform>(entity);
    lua_pushnumber(L, transform.position.x);
    lua_pushnumber(L, transform.position.y);
    lua_pushnumber(L, transform.position.z);
    return 3;
}

// urt.setPosition(id, x, y, z). Works on players too: that is a teleport.
int api_set_position(lua_State* L) {
    const Entity entity = check_entity(L);
    const glm::vec3 position{static_cast<float>(check_number(L, 2, max_coordinate, "x")),
                             static_cast<float>(check_number(L, 3, max_coordinate, "y")),
                             static_cast<float>(check_number(L, 4, max_coordinate, "z"))};
    resource_of(L).host->world().registry.get<Transform>(entity).position = position;
    return 0;
}

// urt.getYaw(id) -> radians
int api_get_yaw(lua_State* L) {
    const Entity entity = check_entity(L);
    lua_pushnumber(L, resource_of(L).host->world().registry.get<Transform>(entity).yaw);
    return 1;
}

// urt.setYaw(id, radians)
int api_set_yaw(lua_State* L) {
    const Entity entity = check_entity(L);
    const double yaw = check_number(L, 2, max_coordinate, "yaw");
    resource_of(L).host->world().registry.get<Transform>(entity).yaw = static_cast<float>(yaw);
    return 0;
}

// urt.setVelocity(id, vx, vy, vz) in metres per second. The entity keeps
// moving until told otherwise. A player's velocity comes from that player's
// input and is overwritten every tick.
int api_set_velocity(lua_State* L) {
    const Entity entity = check_entity(L);
    const glm::vec3 velocity{static_cast<float>(check_number(L, 2, max_speed, "vx")),
                             static_cast<float>(check_number(L, 3, max_speed, "vy")),
                             static_cast<float>(check_number(L, 4, max_speed, "vz"))};
    resource_of(L).host->world().registry.get_or_emplace<Velocity>(entity).linear = velocity;
    return 0;
}

// urt.players() -> array of { id, name, x, y, z }
int api_players(lua_State* L) {
    Host& host = *resource_of(L).host;
    const std::vector<net::PlayerInfo> players = host.server().players();
    lua_createtable(L, static_cast<int>(players.size()), 0);
    int index = 1;
    for (const net::PlayerInfo& player : players) {
        push_player(L, player.id, player.name, host.world().registry.get<Transform>(player.entity).position);
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

// urt.time() -> seconds the server has been simulating
int api_time(lua_State* L) {
    lua_pushnumber(L, resource_of(L).host->time());
    return 1;
}

// --- Host implementation -------------------------------------------------------------

void Host::register_api(Resource& resource) {
    lua_State* L = resource.vm->state();

    // Every function carries a pointer to its resource as an "upvalue", so
    // it knows who is calling without any global lookup.
    const auto add = [&](const char* name, lua_CFunction function) {
        lua_pushlightuserdata(L, &resource);
        lua_pushcclosure(L, function, name, 1);
        lua_setfield(L, -2, name);
    };

    lua_newtable(L);
    add("log", api_log);
    add("on", api_on);
    add("after", api_after);
    add("every", api_every);
    add("clearTimer", api_clear_timer);
    add("spawn", api_spawn);
    add("despawn", api_despawn);
    add("exists", api_exists);
    add("getPosition", api_get_position);
    add("setPosition", api_set_position);
    add("getYaw", api_get_yaw);
    add("setYaw", api_set_yaw);
    add("setVelocity", api_set_velocity);
    add("players", api_players);
    add("time", api_time);
    lua_pushlstring(L, resource.name.data(), resource.name.size());
    lua_setfield(L, -2, "resourceName");
    lua_setglobal(L, "urt");

    // print() goes to the server log too, instead of a console nobody reads.
    lua_pushlightuserdata(L, &resource);
    lua_pushcclosure(L, api_log, "print", 1);
    lua_setglobal(L, "print");
}

// Runs resource.lua in a throwaway VM that has no game API at all, and reads
// the table it returns.
bool Host::read_manifest(Resource& resource, Manifest& manifest) {
    const std::filesystem::path path = resource.directory / manifest_file;
    std::string source;
    if (!read_file(path, source, resource.error)) {
        return false;
    }

    Vm vm(manifest_memory_bytes);
    if (vm.state() == nullptr) {
        resource.error = "cannot create a script VM";
        return false;
    }
    vm.seal();
    if (!vm.load(resource.name + "/" + manifest_file, source, resource.error)) {
        return false;
    }

    std::string problem;
    const auto parse = [&](lua_State* L, int count) {
        if (count < 1 || !lua_istable(L, 1)) {
            problem = "resource.lua must return a table";
            return;
        }
        lua_getfield(L, 1, "name");
        if (!lua_isnil(L, -1) && (!lua_isstring(L, -1) || resource.name != lua_tostring(L, -1))) {
            problem = "the name in resource.lua must be the folder's name, '" + resource.name + "'";
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "server");
        if (lua_istable(L, -1)) {
            const int scripts = lua_objlen(L, -1);
            if (scripts > static_cast<int>(max_scripts_per_resource)) {
                problem = "too many server scripts";
            }
            for (int i = 1; i <= scripts && problem.empty(); ++i) {
                lua_rawgeti(L, -1, i);
                const char* script = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
                if (script == nullptr || !valid_script_path(script)) {
                    problem = "entry " + std::to_string(i) +
                              " of 'server' is not a valid script path (a .lua file inside the resource)";
                } else {
                    manifest.server_scripts.emplace_back(script);
                }
                lua_pop(L, 1);
            }
        } else if (!lua_isnil(L, -1)) {
            problem = "'server' must be a list of script files";
        }
        lua_pop(L, 1);
    };

    const CallResult result = vm.call(0, config_.max_call_seconds, resource.error, parse);
    if (result != CallResult::Ok) {
        return false;
    }
    if (!problem.empty()) {
        resource.error = problem;
        return false;
    }
    return true;
}

bool Host::start(Resource& resource) {
    resource.error.clear();
    resource.watched.clear();
    const std::filesystem::path manifest_path = resource.directory / manifest_file;
    resource.watched.emplace_back(manifest_path, modified_time(manifest_path));

    try {
        Manifest manifest;
        if (!read_manifest(resource, manifest)) {
            fail(resource, resource.error);
            return false;
        }
        // Watch every script before running any, so that fixing a broken
        // file triggers a reload.
        for (const std::string& script : manifest.server_scripts) {
            const std::filesystem::path path = resource.directory / script;
            resource.watched.emplace_back(path, modified_time(path));
        }

        resource.vm = std::make_unique<Vm>(config_.memory_limit_bytes);
        if (resource.vm->state() == nullptr) {
            fail(resource, "cannot create a script VM");
            return false;
        }
        register_api(resource);
        resource.vm->seal();

        for (const std::string& script : manifest.server_scripts) {
            std::string source;
            std::string error;
            if (!read_file(resource.directory / script, source, error) ||
                !resource.vm->load(resource.name + "/" + script, source, error)) {
                fail(resource, error);
                return false;
            }
            // Loading may do one-off setup work, so it gets more time than
            // a handler that runs every tick.
            const CallResult result = resource.vm->call(0, config_.max_call_seconds * 4.0, error);
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

    resource.state = ResourceState::Running;
    log::info("script", "started resource '{}'", resource.name);
    return true;
}

void Host::fail(Resource& resource, std::string error) {
    log::error("script", "[{}] stopped: {}", resource.name, error);
    shut_down(resource);
    resource.state = ResourceState::Failed;
    resource.error = std::move(error);
}

// Removes everything a resource put into the world and frees its VM.
void Host::shut_down(Resource& resource) {
    if (world_ != nullptr) {
        for (const net::NetId id : resource.entities) {
            server_.despawn(*world_, id);
        }
    }
    resource.entities.clear();
    resource.handlers.clear();
    resource.handler_count = 0;
    resource.timers.clear();
    resource.vm.reset(); // closing the VM releases every function reference
    resource.state = ResourceState::Failed;
}

void Host::restart(Resource& resource) {
    if (resource.state == ResourceState::Running) {
        dispatch(resource, "stop");
    }
    shut_down(resource);
    start(resource);
}

// Calls one script function. Returns false if the resource is no longer
// running afterwards.
bool Host::invoke(Resource& resource, int handler, std::string_view what,
                  const std::function<int(lua_State*)>& push_arguments) {
    std::string error;
    CallResult result = CallResult::Killed;
    try {
        lua_State* L = resource.vm->state();
        lua_getref(L, handler);
        const int arguments = push_arguments ? push_arguments(L) : 0;
        result = resource.vm->call(arguments, config_.max_call_seconds, error);
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
    if (seconds > config_.slow_call_seconds && Clock::now() - resource.last_slow_warning > std::chrono::seconds(5)) {
        resource.last_slow_warning = Clock::now();
        log::warn("script", "[{}] {} took {:.1f} ms", resource.name, what, seconds * 1000.0);
    }
    return true;
}

void Host::dispatch(Resource& resource, const char* event, const std::function<int(lua_State*)>& push_arguments) {
    if (resource.state != ResourceState::Running) {
        return;
    }
    const auto found = resource.handlers.find(event);
    if (found == resource.handlers.end()) {
        return;
    }
    // A copy: a handler may register more handlers while we go through them.
    const std::vector<int> handlers = found->second;
    const std::string what = std::string("the '") + event + "' handler";
    for (const int handler : handlers) {
        if (!invoke(resource, handler, what, push_arguments)) {
            return;
        }
    }
}

void Host::run_timers(Resource& resource) {
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

Resource* Host::find(std::string_view name) {
    for (const std::unique_ptr<Resource>& resource : resources_) {
        if (resource->name == name) {
            return resource.get();
        }
    }
    return nullptr;
}

bool Host::load_directory(const std::filesystem::path& directory) {
    const std::string name = directory.filename().string();
    if (!valid_resource_name(name)) {
        log::warn("script", "skipping '{}': resource folder names may only use a-z, 0-9, '_' and '-'", name);
        return false;
    }
    Resource* resource = find(name);
    if (resource != nullptr) {
        resource->directory = directory;
        restart(*resource);
        return resource->state == ResourceState::Running;
    }
    auto created = std::make_unique<Resource>();
    created->host = this;
    created->name = name;
    created->directory = directory;
    resource = created.get();
    resources_.push_back(std::move(created));
    return start(*resource);
}

bool Host::load(World& world, const std::filesystem::path& directory) {
    world_ = &world;
    const bool running = load_directory(directory);
    world_ = nullptr;
    return running;
}

bool Host::stop(World& world, std::string_view name) {
    Resource* resource = find(name);
    if (resource == nullptr) {
        return false;
    }
    world_ = &world;
    if (resource->state == ResourceState::Running) {
        dispatch(*resource, "stop");
        log::info("script", "stopped resource '{}'", resource->name);
    }
    shut_down(*resource);
    world_ = nullptr;
    std::erase_if(resources_, [&](const std::unique_ptr<Resource>& r) { return r.get() == resource; });
    return true;
}

// Looks for resource folders that appeared or vanished and files that changed.
void Host::check_files() {
    std::error_code ignored;

    for (std::size_t i = 0; i < resources_.size();) {
        Resource& resource = *resources_[i];
        if (!std::filesystem::is_directory(resource.directory, ignored)) {
            log::info("script", "resource '{}' was removed from disk", resource.name);
            if (resource.state == ResourceState::Running) {
                dispatch(resource, "stop");
            }
            shut_down(resource);
            resources_.erase(resources_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const bool changed = std::any_of(resource.watched.begin(), resource.watched.end(),
                                         [](const auto& file) { return modified_time(file.first) != file.second; });
        if (changed) {
            log::info("script", "files of resource '{}' changed, reloading", resource.name);
            restart(resource);
        }
        ++i;
    }

    if (config_.resources_directory.empty()) {
        return;
    }
    std::vector<std::filesystem::path> fresh;
    for (const auto& entry : std::filesystem::directory_iterator(config_.resources_directory, ignored)) {
        if (entry.is_directory(ignored) && std::filesystem::exists(entry.path() / manifest_file, ignored) &&
            find(entry.path().filename().string()) == nullptr &&
            valid_resource_name(entry.path().filename().string())) {
            fresh.push_back(entry.path());
        }
    }
    std::sort(fresh.begin(), fresh.end()); // the same start order on every machine
    for (const std::filesystem::path& directory : fresh) {
        load_directory(directory);
    }
}

void Host::tick(World& world, const TickContext& context) {
    world_ = &world;
    time_ = context.time;

    if (!started_) {
        started_ = true;
        std::error_code ignored;
        if (!config_.resources_directory.empty() &&
            !std::filesystem::is_directory(config_.resources_directory, ignored)) {
            log::warn("script", "resources folder '{}' does not exist", config_.resources_directory.string());
        }
        check_files(); // with nothing loaded yet, this loads everything
        last_file_check_ = Clock::now();
    }

    // None of the calls below adds or removes resources: a failing one stays
    // in the list, marked as failed.
    for (const net::PlayerEvent& event : server_.player_events()) {
        const char* name = event.kind == net::PlayerEvent::Kind::Joined ? "playerJoined" : "playerLeft";
        for (const std::unique_ptr<Resource>& resource : resources_) {
            dispatch(*resource, name, [&](lua_State* L) {
                push_player(L, event.id, event.name, event.position);
                return 1;
            });
        }
    }
    for (const std::unique_ptr<Resource>& resource : resources_) {
        run_timers(*resource);
        dispatch(*resource, "tick", [&](lua_State* L) {
            lua_pushnumber(L, context.dt);
            return 1;
        });
    }

    if (config_.hot_reload && std::chrono::duration<double>(Clock::now() - last_file_check_).count() >=
                                  config_.hot_reload_interval_seconds) {
        last_file_check_ = Clock::now();
        check_files();
    }
    world_ = nullptr;
}

std::vector<ResourceInfo> Host::resources() const {
    std::vector<ResourceInfo> result;
    for (const std::unique_ptr<Resource>& resource : resources_) {
        ResourceInfo info;
        info.name = resource->name;
        info.state = resource->state;
        info.error = resource->error;
        info.entities = resource->entities.size();
        info.memory_bytes = resource->vm != nullptr ? resource->vm->memory_used() : 0;
        result.push_back(std::move(info));
    }
    return result;
}

} // namespace

struct ScriptSystem::Impl : Host {
    using Host::Host;
};

ScriptSystem::ScriptSystem(net::Server& server, ScriptConfig config)
    : impl_(std::make_unique<Impl>(server, std::move(config))) {}

ScriptSystem::~ScriptSystem() = default;

void ScriptSystem::tick(World& world, const TickContext& context) { impl_->tick(world, context); }

bool ScriptSystem::load(World& world, const std::filesystem::path& directory) {
    return impl_->load(world, directory);
}

bool ScriptSystem::stop(World& world, std::string_view name) { return impl_->stop(world, name); }

std::vector<ResourceInfo> ScriptSystem::resources() const { return impl_->resources(); }

std::optional<ResourceInfo> ScriptSystem::resource(std::string_view name) const {
    for (ResourceInfo& info : impl_->resources()) {
        if (info.name == name) {
            return std::move(info);
        }
    }
    return std::nullopt;
}

} // namespace urt::script
