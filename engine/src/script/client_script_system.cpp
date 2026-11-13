#include "urt/script/client_script_system.hpp"

#include "host.hpp"
#include "value_codec.hpp"

#include "urt/core/log.hpp"
#include "urt/ecs/components.hpp"
#include "urt/net/client.hpp"
#include "urt/net/components.hpp"

#include <algorithm>

#include <lua.h>
#include <lualib.h>

namespace urt::script {
namespace {

class ClientHost : public HostBase {
public:
    ClientHost(net::Client& client, ClientScriptConfig config)
        : HostBase(HostLimits{config.max_call_seconds, config.slow_call_seconds, config.memory_limit_bytes}),
          client_(client),
          config_(config) {}

    void tick(World& world, const TickContext& context);
    std::vector<ResourceInfo> resources() const;

    const char* side() const override { return "client"; }
    bool known_event(std::string_view event) const override { return event == "tick" || event == "stop"; }

    // Used by the functions scripts call. Only valid while a script runs.
    net::Client& client() { return client_; }
    World& world() { return *world_; }

protected:
    void register_side_api(Resource& resource, lua_State* L) override;
    void release_world(Resource& resource) override;
    void on_failed(Resource&) override {}

private:
    Resource* find(std::string_view name);
    void start(const net::ServerResourceStart& message);
    void stop(std::string_view name);
    void stop_all();
    void end(Resource& resource);

    net::Client& client_;
    ClientScriptConfig config_;
    std::vector<std::unique_ptr<Resource>> resources_;
    World* world_ = nullptr;
};

ClientHost& host_of(lua_State* L) { return static_cast<ClientHost&>(*resource_of(L).host); }

// --- the client's urt.* functions ---------------------------------------------------

// urt.emitServer(event, ...) -> false when not connected
int api_emit_server(lua_State* L) {
    std::size_t length = 0;
    const char* name = luaL_checklstring(L, 1, &length);
    net::NetEvent event;
    event.name.assign(name, length);
    if (!net::valid_event_name(event.name)) {
        luaL_error(L, "an event name is 1 to 64 characters without spaces");
    }
    encode_values(L, 2, lua_gettop(L), event.arguments);
    lua_pushboolean(L, host_of(L).client().send_event(event) ? 1 : 0);
    return 1;
}

// urt.localPlayer() -> the id of our own player, or nil before we are in the game
int api_local_player(lua_State* L) {
    const net::NetId id = host_of(L).client().player_id();
    if (id == 0) {
        lua_pushnil(L);
    } else {
        lua_pushnumber(L, id);
    }
    return 1;
}

// urt.entities() -> array of the ids of every entity we can currently see
int api_entities(lua_State* L) {
    ClientHost& host = host_of(L);
    lua_newtable(L);
    int index = 1;
    for (auto [entity, id] : host.world().registry.view<const net::NetworkId>().each()) {
        lua_pushnumber(L, id.value);
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

// urt.exists(id) -> true if we can currently see that entity
int api_exists(lua_State* L) {
    lua_pushboolean(L, host_of(L).client().find(check_entity_id(L, 1)) != entt::null ? 1 : 0);
    return 1;
}

// Finds the entity for the id in argument 1, or raises a script error.
Entity check_entity(lua_State* L) {
    const net::NetId id = check_entity_id(L, 1);
    const Entity entity = host_of(L).client().find(id);
    if (entity == entt::null) {
        luaL_error(L, "entity %d is not visible to this client", static_cast<int>(id));
    }
    return entity;
}

// urt.getPosition(id) -> x, y, z as last confirmed by the server
int api_get_position(lua_State* L) {
    const Entity entity = check_entity(L);
    const Transform& transform = host_of(L).world().registry.get<Transform>(entity);
    lua_pushnumber(L, transform.position.x);
    lua_pushnumber(L, transform.position.y);
    lua_pushnumber(L, transform.position.z);
    return 3;
}

// urt.getYaw(id) -> radians
int api_get_yaw(lua_State* L) {
    const Entity entity = check_entity(L);
    lua_pushnumber(L, host_of(L).world().registry.get<Transform>(entity).yaw);
    return 1;
}

// urt.setColor(id, r, g, b): draw the entity in this colour, each part 0..1.
// Only this client sees it, and it is forgotten when the entity leaves view.
int api_set_color(lua_State* L) {
    const Entity entity = check_entity(L);
    const net::NetId id = check_entity_id(L, 1);
    glm::vec3 color{0.0f};
    const char* const names[3] = {"red", "green", "blue"};
    for (int i = 0; i < 3; ++i) {
        const double part = check_number(L, 2 + i, 1.0, names[i]);
        if (part < 0.0) {
            luaL_error(L, "%s must be between 0 and 1", names[i]);
        }
        color[i] = static_cast<float>(part);
    }
    host_of(L).world().registry.emplace_or_replace<net::Tint>(entity, color);
    resource_of(L).tinted.insert(id);
    return 0;
}

// urt.clearColor(id): back to the default colour
int api_clear_color(lua_State* L) {
    const net::NetId id = check_entity_id(L, 1);
    const Entity entity = host_of(L).client().find(id);
    if (entity != entt::null) {
        host_of(L).world().registry.remove<net::Tint>(entity);
    }
    resource_of(L).tinted.erase(id);
    return 0;
}

// --- ClientHost implementation --------------------------------------------------------

void ClientHost::register_side_api(Resource& resource, lua_State* L) {
    add_function(L, resource, "emitServer", api_emit_server);
    add_function(L, resource, "localPlayer", api_local_player);
    add_function(L, resource, "entities", api_entities);
    add_function(L, resource, "exists", api_exists);
    add_function(L, resource, "getPosition", api_get_position);
    add_function(L, resource, "getYaw", api_get_yaw);
    add_function(L, resource, "setColor", api_set_color);
    add_function(L, resource, "clearColor", api_clear_color);
}

void ClientHost::release_world(Resource& resource) {
    if (world_ != nullptr) {
        for (const net::NetId id : resource.tinted) {
            const Entity entity = client_.find(id);
            if (entity != entt::null) {
                world_->registry.remove<net::Tint>(entity);
            }
        }
    }
    resource.tinted.clear();
}

Resource* ClientHost::find(std::string_view name) {
    for (const std::unique_ptr<Resource>& resource : resources_) {
        if (resource->name == name) {
            return resource.get();
        }
    }
    return nullptr;
}

void ClientHost::end(Resource& resource) {
    if (resource.state == ResourceState::Running) {
        dispatch(resource, "stop");
    }
    shut_down(resource);
}

void ClientHost::start(const net::ServerResourceStart& message) {
    Resource* resource = find(message.resource);
    if (resource != nullptr) {
        end(*resource); // a newer version replaces the one we run
    } else {
        if (resources_.size() >= config_.max_resources) {
            log::warn("script", "the server sent more than {} resources; ignoring '{}'", config_.max_resources,
                      message.resource);
            return;
        }
        auto created = std::make_unique<Resource>();
        created->host = this;
        created->name = message.resource;
        resource = created.get();
        resources_.push_back(std::move(created));
    }
    run_scripts(*resource, message.scripts);
}

void ClientHost::stop(std::string_view name) {
    Resource* resource = find(name);
    if (resource == nullptr) {
        return;
    }
    if (resource->state == ResourceState::Running) {
        log::info("script", "stopping client resource '{}'", resource->name);
    }
    end(*resource);
    std::erase_if(resources_, [&](const std::unique_ptr<Resource>& r) { return r.get() == resource; });
}

void ClientHost::stop_all() {
    for (const std::unique_ptr<Resource>& resource : resources_) {
        end(*resource);
    }
    resources_.clear();
}

void ClientHost::tick(World& world, const TickContext& context) {
    world_ = &world;
    time_ = context.time;

    for (const net::ScriptMessage& message : client_.take_script_messages()) {
        switch (message.kind) {
        case net::ScriptMessage::Kind::Reset:
            stop_all();
            break;
        case net::ScriptMessage::Kind::ResourceStart:
            start(message.start);
            break;
        case net::ScriptMessage::Kind::ResourceStop:
            stop(message.resource);
            break;
        case net::ScriptMessage::Kind::Event:
            for (const std::unique_ptr<Resource>& resource : resources_) {
                dispatch_net(*resource, message.event.name, [&](lua_State* L) {
                    return decode_values(L, message.event.arguments); // -1 skips the call
                });
            }
            break;
        }
    }

    for (const std::unique_ptr<Resource>& resource : resources_) {
        run_timers(*resource);
        dispatch(*resource, "tick", [&](lua_State* L) {
            lua_pushnumber(L, context.dt);
            return 1;
        });
    }
    world_ = nullptr;
}

std::vector<ResourceInfo> ClientHost::resources() const {
    std::vector<ResourceInfo> result;
    for (const std::unique_ptr<Resource>& resource : resources_) {
        ResourceInfo info;
        info.name = resource->name;
        info.state = resource->state;
        info.error = resource->error;
        info.memory_bytes = resource->vm != nullptr ? resource->vm->memory_used() : 0;
        result.push_back(std::move(info));
    }
    return result;
}

} // namespace

struct ClientScriptSystem::Impl : ClientHost {
    using ClientHost::ClientHost;
};

ClientScriptSystem::ClientScriptSystem(net::Client& client, ClientScriptConfig config)
    : impl_(std::make_unique<Impl>(client, config)) {}

ClientScriptSystem::~ClientScriptSystem() = default;

void ClientScriptSystem::tick(World& world, const TickContext& context) { impl_->tick(world, context); }

std::vector<ResourceInfo> ClientScriptSystem::resources() const { return impl_->resources(); }

std::optional<ResourceInfo> ClientScriptSystem::resource(std::string_view name) const {
    for (ResourceInfo& info : impl_->resources()) {
        if (info.name == name) {
            return std::move(info);
        }
    }
    return std::nullopt;
}

} // namespace urt::script
