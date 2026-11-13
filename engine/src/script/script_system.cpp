#include "urt/script/script_system.hpp"

#include "host.hpp"
#include "value_codec.hpp"

#include "urt/core/log.hpp"
#include "urt/ecs/components.hpp"
#include "urt/net/server.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <system_error>

#include <lua.h>
#include <lualib.h>

namespace urt::script {
namespace {

constexpr const char* manifest_file = "resource.lua";
constexpr std::size_t max_server_scripts = 256;
constexpr double max_coordinate = 1.0e6; // metres from the origin
constexpr double max_speed = 1000.0;     // metres per second
constexpr std::size_t manifest_memory_bytes = 4u * 1024u * 1024u;

struct Manifest {
    std::vector<std::string> server_scripts;
    std::vector<std::string> client_scripts;
};

// A script path from a manifest must stay inside the resource's folder.
bool valid_script_path(std::string_view path) {
    if (path.empty() || path.size() > 150 || path.front() == '/' || path.find("..") != std::string_view::npos) {
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
    if (out.size() > net::max_script_bytes) {
        error = path.filename().string() + " is larger than 1 MB";
        return false;
    }
    return true;
}

std::filesystem::file_time_type modified_time(const std::filesystem::path& path) {
    std::error_code ignored;
    return std::filesystem::last_write_time(path, ignored); // the minimum time if the file is gone
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

// --- the server's script host -----------------------------------------------------

class ServerHost : public HostBase {
public:
    ServerHost(net::Server& server, ScriptConfig config)
        : HostBase(HostLimits{config.max_call_seconds, config.slow_call_seconds, config.memory_limit_bytes}),
          server_(server),
          config_(std::move(config)) {}

    void tick(World& world, const TickContext& context);
    bool load(World& world, const std::filesystem::path& directory);
    bool stop(World& world, std::string_view name);
    std::vector<ResourceInfo> resources() const;

    const char* side() const override { return "server"; }
    bool known_event(std::string_view event) const override {
        return event == "playerJoined" || event == "playerLeft" || event == "tick" || event == "stop";
    }

    // Used by the functions scripts call. Only valid while a script runs.
    net::Server& server() { return server_; }
    World& world() { return *world_; }
    const ScriptConfig& config() const { return config_; }

protected:
    void register_side_api(Resource& resource, lua_State* L) override;
    void release_world(Resource& resource) override;
    void on_failed(Resource& resource) override { tell_clients_to_stop(resource); }

private:
    Resource* find(std::string_view name);
    bool load_directory(const std::filesystem::path& directory);
    bool start(Resource& resource);
    bool read_manifest(Resource& resource, Manifest& manifest);
    void restart(Resource& resource);
    void end(Resource& resource);
    void check_files();

    void send_client_scripts(const Resource& resource, net::NetId player);
    void tell_clients_to_start(Resource& resource);
    void tell_clients_to_stop(Resource& resource);

    net::Server& server_;
    ScriptConfig config_;
    // unique_ptr: scripts hold pointers to their Resource, so it must not move.
    std::vector<std::unique_ptr<Resource>> resources_;
    World* world_ = nullptr;
    bool started_ = false;
    Clock::time_point last_file_check_{};
};

ServerHost& host_of(lua_State* L) { return static_cast<ServerHost&>(*resource_of(L).host); }

// --- the server's urt.* functions -------------------------------------------------

// urt.spawn({ x, y, z, yaw }) -> entity id. Every field is optional.
int api_spawn(lua_State* L) {
    Resource& resource = resource_of(L);
    ServerHost& host = host_of(L);

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
    ServerHost& host = host_of(L);
    const net::NetId id = check_entity_id(L, 1);
    if (!resource.entities.contains(id)) {
        luaL_error(L, "entity %d was not spawned by this resource", static_cast<int>(id));
    }
    resource.entities.erase(id);
    lua_pushboolean(L, host.server().despawn(host.world(), id) ? 1 : 0);
    return 1;
}

// urt.exists(id) -> boolean
int api_exists(lua_State* L) {
    ServerHost& host = host_of(L);
    lua_pushboolean(L, host.server().find(host.world(), check_entity_id(L, 1)) != entt::null ? 1 : 0);
    return 1;
}

// Finds the entity for the id in argument 1, or raises a script error.
Entity check_entity(lua_State* L) {
    ServerHost& host = host_of(L);
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
    const Transform& transform = host_of(L).world().registry.get<Transform>(entity);
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
    host_of(L).world().registry.get<Transform>(entity).position = position;
    return 0;
}

// urt.getYaw(id) -> radians
int api_get_yaw(lua_State* L) {
    const Entity entity = check_entity(L);
    lua_pushnumber(L, host_of(L).world().registry.get<Transform>(entity).yaw);
    return 1;
}

// urt.setYaw(id, radians)
int api_set_yaw(lua_State* L) {
    const Entity entity = check_entity(L);
    const double yaw = check_number(L, 2, max_coordinate, "yaw");
    host_of(L).world().registry.get<Transform>(entity).yaw = static_cast<float>(yaw);
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
    host_of(L).world().registry.get_or_emplace<Velocity>(entity).linear = velocity;
    return 0;
}

// urt.players() -> array of { id, name, x, y, z }
int api_players(lua_State* L) {
    ServerHost& host = host_of(L);
    const std::vector<net::PlayerInfo> players = host.server().players();
    lua_createtable(L, static_cast<int>(players.size()), 0);
    int index = 1;
    for (const net::PlayerInfo& player : players) {
        push_player(L, player.id, player.name, host.world().registry.get<Transform>(player.entity).position);
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

// Builds the message for urt.emitClient / urt.emitAllClients from the event
// name at `name_index` and every argument after it.
void encode_event(lua_State* L, int name_index, net::ByteWriter& writer) {
    std::size_t length = 0;
    const char* name = luaL_checklstring(L, name_index, &length);
    net::NetEvent event;
    event.name.assign(name, length);
    if (!net::valid_event_name(event.name)) {
        luaL_error(L, "an event name is 1 to 64 characters without spaces");
    }
    encode_values(L, name_index + 1, lua_gettop(L), event.arguments);
    encode(writer, event);
}

// urt.emitClient(playerId, event, ...) -> false if that player is not connected
int api_emit_client(lua_State* L) {
    const net::NetId player = check_entity_id(L, 1);
    net::ByteWriter writer;
    encode_event(L, 2, writer);
    lua_pushboolean(L, host_of(L).server().send_reliable(player, writer.bytes()) ? 1 : 0);
    return 1;
}

// urt.emitAllClients(event, ...)
int api_emit_all_clients(lua_State* L) {
    net::ByteWriter writer;
    encode_event(L, 1, writer);
    host_of(L).server().broadcast_reliable(writer.bytes());
    return 0;
}

// --- ServerHost implementation ------------------------------------------------------

void ServerHost::register_side_api(Resource& resource, lua_State* L) {
    add_function(L, resource, "spawn", api_spawn);
    add_function(L, resource, "despawn", api_despawn);
    add_function(L, resource, "exists", api_exists);
    add_function(L, resource, "getPosition", api_get_position);
    add_function(L, resource, "setPosition", api_set_position);
    add_function(L, resource, "getYaw", api_get_yaw);
    add_function(L, resource, "setYaw", api_set_yaw);
    add_function(L, resource, "setVelocity", api_set_velocity);
    add_function(L, resource, "players", api_players);
    add_function(L, resource, "emitClient", api_emit_client);
    add_function(L, resource, "emitAllClients", api_emit_all_clients);
}

void ServerHost::release_world(Resource& resource) {
    if (world_ != nullptr) {
        for (const net::NetId id : resource.entities) {
            server_.despawn(*world_, id);
        }
    }
    resource.entities.clear();
}

// Runs resource.lua in a throwaway VM that has no game API at all, and reads
// the table it returns.
bool ServerHost::read_manifest(Resource& resource, Manifest& manifest) {
    std::string source;
    if (!read_file(resource.directory / manifest_file, source, resource.error)) {
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
    // Reads a field that must be a list of script paths, such as `server`.
    const auto read_list = [&](lua_State* L, const char* field, std::size_t limit, std::vector<std::string>& out) {
        lua_getfield(L, 1, field);
        if (lua_istable(L, -1)) {
            const int count = lua_objlen(L, -1);
            if (count > static_cast<int>(limit)) {
                problem = std::string("too many scripts in '") + field + "'";
            }
            for (int i = 1; i <= count && problem.empty(); ++i) {
                lua_rawgeti(L, -1, i);
                const char* script = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
                if (script == nullptr || !valid_script_path(script)) {
                    problem = "entry " + std::to_string(i) + " of '" + field +
                              "' is not a valid script path (a .lua file inside the resource)";
                } else {
                    out.emplace_back(script);
                }
                lua_pop(L, 1);
            }
        } else if (!lua_isnil(L, -1)) {
            problem = std::string("'") + field + "' must be a list of script files";
        }
        lua_pop(L, 1);
    };

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
        if (problem.empty()) {
            read_list(L, "server", max_server_scripts, manifest.server_scripts);
        }
        if (problem.empty()) {
            read_list(L, "client", net::max_client_scripts, manifest.client_scripts);
        }
    };

    const CallResult result = vm.call(0, limits_.max_call_seconds, resource.error, parse);
    if (result != CallResult::Ok) {
        return false;
    }
    if (!problem.empty()) {
        resource.error = problem;
        return false;
    }
    return true;
}

bool ServerHost::start(Resource& resource) {
    resource.error.clear();
    resource.client_scripts.clear();
    resource.watched.clear();
    const std::filesystem::path manifest_path = resource.directory / manifest_file;
    resource.watched.emplace_back(manifest_path, modified_time(manifest_path));

    Manifest manifest;
    try {
        if (!read_manifest(resource, manifest)) {
            fail(resource, resource.error);
            return false;
        }
    } catch (const std::exception& e) {
        fail(resource, e.what());
        return false;
    }

    // Watch every script before running any, so that fixing a broken file
    // triggers a reload.
    for (const std::vector<std::string>* list : {&manifest.server_scripts, &manifest.client_scripts}) {
        for (const std::string& script : *list) {
            const std::filesystem::path path = resource.directory / script;
            resource.watched.emplace_back(path, modified_time(path));
        }
    }

    // Read everything first: a resource with a missing file should not run
    // half of its scripts.
    std::vector<net::ScriptFile> server_scripts;
    for (const std::vector<std::string>* list : {&manifest.server_scripts, &manifest.client_scripts}) {
        const bool for_client = list == &manifest.client_scripts;
        for (const std::string& script : *list) {
            net::ScriptFile file;
            file.name = resource.name + "/" + script;
            std::string error;
            if (!read_file(resource.directory / script, file.source, error)) {
                resource.client_scripts.clear();
                fail(resource, error);
                return false;
            }
            (for_client ? resource.client_scripts : server_scripts).push_back(std::move(file));
        }
    }

    if (!run_scripts(resource, server_scripts)) {
        return false;
    }
    tell_clients_to_start(resource);
    return true;
}

// Stops a running resource in an orderly way: its "stop" handlers run, the
// clients are told, and what it spawned is removed.
void ServerHost::end(Resource& resource) {
    if (resource.state == ResourceState::Running) {
        dispatch(resource, "stop");
    }
    tell_clients_to_stop(resource);
    shut_down(resource);
}

void ServerHost::restart(Resource& resource) {
    end(resource);
    start(resource);
}

// Sends one player the client scripts of a resource.
void ServerHost::send_client_scripts(const Resource& resource, net::NetId player) {
    net::ServerResourceStart message;
    message.resource = resource.name;
    message.scripts = resource.client_scripts;
    net::ByteWriter writer;
    encode(writer, message);
    server_.send_reliable(player, writer.bytes());
}

void ServerHost::tell_clients_to_start(Resource& resource) {
    if (resource.client_scripts.empty()) {
        return;
    }
    net::ServerResourceStart message;
    message.resource = resource.name;
    message.scripts = resource.client_scripts;
    net::ByteWriter writer;
    encode(writer, message);
    server_.broadcast_reliable(writer.bytes());
    resource.clients_have_it = true;
}

void ServerHost::tell_clients_to_stop(Resource& resource) {
    if (!resource.clients_have_it) {
        return;
    }
    net::ByteWriter writer;
    encode(writer, net::ServerResourceStop{resource.name});
    server_.broadcast_reliable(writer.bytes());
    resource.clients_have_it = false;
}

Resource* ServerHost::find(std::string_view name) {
    for (const std::unique_ptr<Resource>& resource : resources_) {
        if (resource->name == name) {
            return resource.get();
        }
    }
    return nullptr;
}

bool ServerHost::load_directory(const std::filesystem::path& directory) {
    const std::string name = directory.filename().string();
    if (!net::valid_resource_name(name)) {
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

bool ServerHost::load(World& world, const std::filesystem::path& directory) {
    world_ = &world;
    const bool running = load_directory(directory);
    world_ = nullptr;
    return running;
}

bool ServerHost::stop(World& world, std::string_view name) {
    Resource* resource = find(name);
    if (resource == nullptr) {
        return false;
    }
    world_ = &world;
    if (resource->state == ResourceState::Running) {
        log::info("script", "stopping resource '{}'", resource->name);
    }
    end(*resource);
    world_ = nullptr;
    std::erase_if(resources_, [&](const std::unique_ptr<Resource>& r) { return r.get() == resource; });
    return true;
}

// Looks for resource folders that appeared or vanished and files that changed.
void ServerHost::check_files() {
    std::error_code ignored;

    for (std::size_t i = 0; i < resources_.size();) {
        Resource& resource = *resources_[i];
        if (!std::filesystem::is_directory(resource.directory, ignored)) {
            log::info("script", "resource '{}' was removed from disk", resource.name);
            end(resource);
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
            net::valid_resource_name(entry.path().filename().string())) {
            fresh.push_back(entry.path());
        }
    }
    std::sort(fresh.begin(), fresh.end()); // the same start order on every machine
    for (const std::filesystem::path& directory : fresh) {
        load_directory(directory);
    }
}

void ServerHost::tick(World& world, const TickContext& context) {
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
        const bool joined = event.kind == net::PlayerEvent::Kind::Joined;
        if (joined) {
            // Scripts first: anything a playerJoined handler sends to this
            // client then arrives after the scripts that will receive it.
            for (const std::unique_ptr<Resource>& resource : resources_) {
                if (resource->clients_have_it) {
                    send_client_scripts(*resource, event.id);
                }
            }
        }
        for (const std::unique_ptr<Resource>& resource : resources_) {
            dispatch(*resource, joined ? "playerJoined" : "playerLeft", [&](lua_State* L) {
                push_player(L, event.id, event.name, event.position);
                return 1;
            });
        }
    }

    // Events from client scripts. Whatever they carry was chosen by the
    // client; handlers must check it like any other outside input.
    for (const net::ReceivedEvent& received : server_.received_events()) {
        const Entity entity = server_.player_entity(received.player);
        if (entity == entt::null) {
            continue; // the player left in the same tick
        }
        std::string player_name;
        for (const net::PlayerInfo& player : server_.players()) {
            if (player.id == received.player) {
                player_name = player.name;
            }
        }
        const glm::vec3 position = world.registry.get<Transform>(entity).position;
        for (const std::unique_ptr<Resource>& resource : resources_) {
            dispatch_net(*resource, received.event.name, [&](lua_State* L) {
                push_player(L, received.player, player_name, position);
                const int count = decode_values(L, received.event.arguments);
                if (count < 0) {
                    lua_pop(L, 1); // the player table
                    return -1;     // malformed arguments: skip the call
                }
                return 1 + count;
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

std::vector<ResourceInfo> ServerHost::resources() const {
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

struct ScriptSystem::Impl : ServerHost {
    using ServerHost::ServerHost;
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
