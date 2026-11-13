#include "urt/net/client.hpp"

#include "urt/core/log.hpp"
#include "urt/ecs/components.hpp"
#include "urt/net/components.hpp"
#include "urt/net/interpolation.hpp"

#include <utility>

namespace urt::net {
namespace {

const char* reject_text(RejectReason reason) {
    switch (reason) {
    case RejectReason::VersionMismatch: return "client and server versions differ";
    case RejectReason::ServerFull:      return "the server is full";
    case RejectReason::BadName:         return "the player name is not allowed";
    }
    return "unknown reason";
}

} // namespace

Client::Client(ClientConfig config) : config_(std::move(config)) {}

bool Client::connect() {
    if (state_ != ClientState::Disconnected) {
        return false;
    }
    transport_.close();
    reject_reason_.reset();
    disconnect_requested_ = false;

    server_peer_ = transport_.connect(config_.host, config_.port);
    if (server_peer_ == 0) {
        return false;
    }
    state_ = ClientState::Connecting;
    log::info("net", "connecting to {}:{} as '{}'", config_.host, config_.port, config_.name);
    return true;
}

void Client::disconnect() { disconnect_requested_ = true; }

Entity Client::find(NetId id) const {
    const auto found = entities_.find(id);
    return found != entities_.end() ? found->second.entity : Entity{entt::null};
}

void Client::set_input(float move_x, float move_z, float yaw) {
    input_.move_x = move_x;
    input_.move_z = move_z;
    input_.yaw = yaw;
}

void Client::tick(World& world, const TickContext&) {
    if (disconnect_requested_) {
        disconnect_requested_ = false;
        transport_.close(); // tells the server right away
        clear_world(world);
        if (state_ != ClientState::Disconnected) {
            log::info("net", "disconnected");
        }
        state_ = ClientState::Disconnected;
        return;
    }
    if (state_ == ClientState::Disconnected) {
        return;
    }

    events_.clear();
    transport_.poll(events_);

    for (const Event& event : events_) {
        switch (event.type) {
        case Event::Type::Connected:
            // The UDP connection is up; now introduce ourselves.
            writer_.clear();
            encode(writer_, ClientHello{.version = protocol_version, .name = config_.name});
            transport_.send(server_peer_, Channel::Reliable, writer_.bytes());
            state_ = ClientState::Handshaking;
            break;
        case Event::Type::Disconnected:
            if (reject_reason_) {
                log::error("net", "server refused the connection: {}", reject_text(*reject_reason_));
            } else if (state_ == ClientState::Connecting) {
                log::error("net", "no answer from {}:{}", config_.host, config_.port);
            } else {
                log::warn("net", "connection to the server was lost");
            }
            clear_world(world);
            state_ = ClientState::Disconnected;
            break;
        case Event::Type::Received:
            handle_packet(world, event.channel, event.data);
            break;
        }
    }

    if (state_ == ClientState::Connected) {
        ++input_.sequence;
        writer_.clear();
        encode(writer_, input_);
        transport_.send(server_peer_, Channel::Unreliable, writer_.bytes());
    }
    transport_.flush();
}

void Client::handle_packet(World& world, Channel channel, std::span<const std::uint8_t> data) {
    switch (message_type(data)) {
    case MessageType::ServerWelcome: {
        ServerWelcome welcome;
        if (channel != Channel::Reliable || state_ != ClientState::Handshaking ||
            !decode(data, welcome)) {
            return;
        }
        player_id_ = welcome.player_id;
        server_tick_rate_ = welcome.tick_rate;
        state_ = ClientState::Connected;
        log::info("net", "connected as player {} (server runs at {} ticks/s)", player_id_,
                  server_tick_rate_);
        return;
    }
    case MessageType::ServerReject: {
        ServerReject rejection;
        if (channel == Channel::Reliable && decode(data, rejection)) {
            reject_reason_ = rejection.reason; // the disconnect event follows
        }
        return;
    }
    case MessageType::ServerSnapshot: {
        // The two channels are independent, so a snapshot can overtake the
        // welcome. Ignore snapshots until we know who we are.
        if (channel != Channel::Unreliable || state_ != ClientState::Connected ||
            !decode(data, snapshot_)) {
            return;
        }
        if (has_snapshot_ && !sequence_newer(snapshot_.server_tick, last_snapshot_tick_)) {
            return; // older than the one we already applied
        }
        apply_snapshot(world, snapshot_);
        return;
    }
    case MessageType::ClientHello:
    case MessageType::ClientInput:
    case MessageType::Invalid:
        return;
    }
}

void Client::apply_snapshot(World& world, const ServerSnapshot& snapshot) {
    auto& registry = world.registry;
    const double snapshot_time = static_cast<double>(snapshot.server_tick) / server_tick_rate_;

    for (const SnapshotEntity& remote : snapshot.entities) {
        Replicated& local = entities_[remote.id];
        if (local.entity == entt::null) {
            // First time we see this entity: create its local twin.
            local.entity = registry.create();
            registry.emplace<NetworkId>(local.entity, remote.id);
            registry.emplace<Transform>(local.entity);
            registry.emplace<Velocity>(local.entity);
            registry.emplace<PoseHistory>(local.entity);
            if (remote.id == player_id_) {
                registry.emplace<LocalPlayer>(local.entity);
                player_entity_ = local.entity;
            }
        }
        local.seen_tick = snapshot.server_tick;

        Transform& transform = registry.get<Transform>(local.entity);
        transform.position = remote.position;
        transform.yaw = remote.yaw;
        registry.get<Velocity>(local.entity).linear = remote.velocity;
        // Remember the pose with its time, for smooth drawing between snapshots.
        registry.get<PoseHistory>(local.entity)
            .push(snapshot_time, Pose{.position = remote.position, .yaw = remote.yaw});
    }

    // Whatever the snapshot no longer mentions has left our surroundings.
    for (auto it = entities_.begin(); it != entities_.end();) {
        if (it->second.seen_tick != snapshot.server_tick) {
            if (it->second.entity == player_entity_) {
                player_entity_ = entt::null;
            }
            registry.destroy(it->second.entity);
            it = entities_.erase(it);
        } else {
            ++it;
        }
    }

    last_snapshot_tick_ = snapshot.server_tick;
    has_snapshot_ = true;
    ++snapshots_received_;
}

void Client::clear_world(World& world) {
    for (const auto& [id, replicated] : entities_) {
        if (world.registry.valid(replicated.entity)) {
            world.registry.destroy(replicated.entity);
        }
    }
    entities_.clear();
    player_entity_ = entt::null;
    player_id_ = 0;
    has_snapshot_ = false;
}

} // namespace urt::net
