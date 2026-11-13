#include "urt/net/server.hpp"

#include "urt/core/log.hpp"
#include "urt/ecs/components.hpp"
#include "urt/net/components.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace urt::net {
namespace {

// Extra connection slots beyond max_players, so a full server can still
// accept a connection long enough to say "server full".
constexpr std::size_t spare_slots = 8;

std::uint16_t tick_rate_of(const TickContext& context) {
    return static_cast<std::uint16_t>(std::clamp(std::lround(1.0 / context.dt), 1L, 65535L));
}

// Names may contain any printable text, but no control characters: they would
// let a player forge lines in logs and, later, in chat.
bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > max_name_length) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte >= 0x20 && byte != 0x7F;
    });
}

// New players appear on a small spiral around the origin so they do not
// stand inside each other.
glm::vec3 spawn_point(NetId id) {
    const float angle = static_cast<float>(id) * 2.4f;
    const float radius = 2.0f + 0.5f * std::sqrt(static_cast<float>(id));
    return {std::cos(angle) * radius, 0.0f, std::sin(angle) * radius};
}

} // namespace

Server::Server(ServerConfig config)
    : config_(config), grid_(std::max(config.interest_radius / 3.0f, 1.0f)) {
    config_.snapshot_interval = std::max<std::uint32_t>(config_.snapshot_interval, 1);
}

bool Server::start() {
    if (!transport_.listen(config_.port, config_.max_players + spare_slots)) {
        return false;
    }
    log::info("net", "listening on UDP port {} for up to {} players", transport_.port(),
              config_.max_players);
    return true;
}

NetId Server::make_networked(World& world, Entity entity) {
    const NetId id = next_net_id_++;
    world.registry.emplace_or_replace<NetworkId>(entity, id);
    return id;
}

Entity Server::player_entity(NetId player_id) const {
    for (const auto& [peer, session] : sessions_) {
        if (session.active && session.player_id == player_id) {
            return session.entity;
        }
    }
    return entt::null;
}

void Server::tick(World& world, const TickContext& context) {
    events_.clear();
    transport_.poll(events_);

    for (const Event& event : events_) {
        switch (event.type) {
        case Event::Type::Connected: {
            Session session;
            session.connected_tick = context.tick;
            sessions_.emplace(event.peer, std::move(session));
            log::debug("net", "peer {} connected, waiting for hello", event.peer);
            break;
        }
        case Event::Type::Disconnected:
            remove_session(world, event.peer);
            break;
        case Event::Type::Received: {
            const auto found = sessions_.find(event.peer);
            if (found != sessions_.end() && !found->second.kicked) {
                handle_packet(world, context, event.peer, found->second, event.channel, event.data);
            }
            break;
        }
        }
    }

    // A connection that never says hello only wastes a slot.
    for (auto& [peer, session] : sessions_) {
        if (session.active || session.kicked) {
            continue;
        }
        const double waited = static_cast<double>(context.tick - session.connected_tick) * context.dt;
        if (waited > config_.hello_timeout_seconds) {
            kick(world, peer, session, "no hello");
        }
    }

    apply_inputs(world, context);
}

void Server::handle_packet(World& world, const TickContext& context, PeerId peer, Session& session,
                           Channel channel, std::span<const std::uint8_t> data) {
    // Every message is only valid on its own channel and in the right state.
    // Anything else means a broken or hostile client, so drop it.
    switch (message_type(data)) {
    case MessageType::ClientHello:
        if (channel != Channel::Reliable || session.active) {
            kick(world, peer, session, "unexpected hello");
            return;
        }
        handle_hello(world, context, peer, session, data);
        return;
    case MessageType::ClientInput:
        if (channel != Channel::Unreliable || !session.active) {
            kick(world, peer, session, "unexpected input");
            return;
        }
        handle_input(world, context, session, data);
        return;
    case MessageType::ServerWelcome:
    case MessageType::ServerReject:
    case MessageType::ServerSnapshot:
    case MessageType::Invalid:
        break;
    }
    kick(world, peer, session, "unknown message");
}

void Server::handle_hello(World& world, const TickContext& context, PeerId peer, Session& session,
                          std::span<const std::uint8_t> data) {
    ClientHello hello;
    if (!decode(data, hello)) {
        kick(world, peer, session, "malformed hello");
        return;
    }
    if (hello.version != protocol_version) {
        reject(peer, session, RejectReason::VersionMismatch);
        return;
    }
    if (!valid_name(hello.name)) {
        reject(peer, session, RejectReason::BadName);
        return;
    }
    if (player_count_ >= config_.max_players) {
        reject(peer, session, RejectReason::ServerFull);
        return;
    }

    // Accepted: create the entity this player controls.
    auto& registry = world.registry;
    const Entity entity = registry.create();
    const NetId id = make_networked(world, entity);
    registry.emplace<Transform>(entity, Transform{.position = spawn_point(id)});
    registry.emplace<Velocity>(entity);
    registry.emplace<PlayerInput>(entity, PlayerInput{.received_tick = context.tick});

    session.active = true;
    session.entity = entity;
    session.player_id = id;
    session.name = std::move(hello.name);
    ++player_count_;

    writer_.clear();
    encode(writer_, ServerWelcome{.player_id = id,
                                  .tick_rate = tick_rate_of(context),
                                  .server_tick = static_cast<std::uint32_t>(context.tick)});
    transport_.send(peer, Channel::Reliable, writer_.bytes());

    log::info("net", "'{}' joined as player {} ({} online)", session.name, id, player_count_);
}

void Server::handle_input(World& world, const TickContext& context, Session& session,
                          std::span<const std::uint8_t> data) {
    ClientInput input;
    if (!decode(data, input)) {
        // Unreliable packets are never resent, so a damaged one is simply
        // skipped. The next input replaces it a tick later.
        return;
    }
    if (session.has_input && !sequence_newer(input.sequence, session.last_input_sequence)) {
        return; // older than what we already have
    }
    session.has_input = true;
    session.last_input_sequence = input.sequence;

    // The decoder guarantees finite numbers; the ranges are enforced here.
    // The client only states a direction. Speed is decided by the server, so
    // a modified client cannot move faster than anyone else.
    glm::vec2 move{std::clamp(input.move_x, -1.0f, 1.0f), std::clamp(input.move_z, -1.0f, 1.0f)};
    const float length = std::sqrt(move.x * move.x + move.y * move.y);
    if (length > 1.0f) {
        move /= length;
    }

    PlayerInput& stored = world.registry.get<PlayerInput>(session.entity);
    stored.move = move;
    stored.yaw = std::remainder(input.yaw, 2.0f * std::numbers::pi_v<float>);
    stored.received_tick = context.tick;
}

void Server::apply_inputs(World& world, const TickContext& context) {
    auto view = world.registry.view<const PlayerInput, Velocity, Transform>();
    for (auto [entity, input, velocity, transform] : view.each()) {
        const double age = static_cast<double>(context.tick - input.received_tick) * context.dt;
        // No fresh input (lag, or the client froze): stand still rather than
        // keep walking in the last direction forever.
        const glm::vec2 move = age > config_.input_timeout_seconds ? glm::vec2{0.0f} : input.move;

        velocity.linear = {move.x * config_.walk_speed, 0.0f, move.y * config_.walk_speed};
        transform.yaw = input.yaw;
    }
}

void Server::despawn(World& world, Session& session) {
    if (!session.active) {
        return;
    }
    if (world.registry.valid(session.entity)) {
        world.registry.destroy(session.entity);
    }
    session.active = false;
    session.entity = entt::null;
    --player_count_;
    log::info("net", "'{}' (player {}) left ({} online)", session.name, session.player_id,
              player_count_);
}

void Server::remove_session(World& world, PeerId peer) {
    const auto found = sessions_.find(peer);
    if (found == sessions_.end()) {
        return;
    }
    despawn(world, found->second);
    sessions_.erase(found);
}

void Server::reject(PeerId peer, Session& session, RejectReason reason) {
    writer_.clear();
    encode(writer_, ServerReject{.reason = reason});
    transport_.send(peer, Channel::Reliable, writer_.bytes());
    log::info("net", "rejected peer {} (reason {})", peer, static_cast<int>(reason));
    session.kicked = true;
    transport_.disconnect(peer); // delivers the reject first, then disconnects
}

void Server::kick(World& world, PeerId peer, Session& session, std::string_view why) {
    log::warn("net", "dropping peer {}: {}", peer, why);
    // Take the player out of the world now. Closing the connection needs the
    // other side to answer, and a hostile client could delay that for seconds.
    despawn(world, session);
    session.kicked = true;
    transport_.disconnect(peer);
}

void Server::send_snapshots(World& world, const TickContext& context) {
    if (context.tick % config_.snapshot_interval != 0) {
        return;
    }
    if (player_count_ == 0) {
        transport_.flush(); // still deliver pending rejects and disconnects
        return;
    }

    // Collect everything replicated once, and index it by position.
    auto& registry = world.registry;
    candidates_.clear();
    grid_.clear();
    for (auto [entity, net_id, transform] : registry.view<const NetworkId, const Transform>().each()) {
        const Velocity* velocity = registry.try_get<Velocity>(entity);
        grid_.insert(static_cast<std::uint32_t>(candidates_.size()), transform.position);
        candidates_.push_back(SnapshotEntity{
            .id = net_id.value,
            .position = transform.position,
            .yaw = transform.yaw,
            .velocity = velocity != nullptr ? velocity->linear : glm::vec3{0.0f},
        });
    }

    // Then build one snapshot per player from what is near that player.
    for (const auto& [peer, session] : sessions_) {
        if (!session.active) {
            continue;
        }
        const glm::vec3 center = registry.get<Transform>(session.entity).position;
        nearby_.clear();
        grid_.query(center, config_.interest_radius, nearby_);

        if (nearby_.size() > max_snapshot_entities) {
            // Too crowded for one snapshot: keep the closest ones. The player's
            // own entity is at distance zero, so it always stays.
            const auto distance_squared = [&](std::uint32_t index) {
                const glm::vec3 d = candidates_[index].position - center;
                return d.x * d.x + d.z * d.z;
            };
            std::nth_element(nearby_.begin(),
                             nearby_.begin() + static_cast<std::ptrdiff_t>(max_snapshot_entities),
                             nearby_.end(), [&](std::uint32_t a, std::uint32_t b) {
                                 return distance_squared(a) < distance_squared(b);
                             });
            nearby_.resize(max_snapshot_entities);
            if (!warned_snapshot_overflow_) {
                warned_snapshot_overflow_ = true;
                log::warn("net", "more than {} entities near one player; sending the closest only",
                          max_snapshot_entities);
            }
        }

        snapshot_.server_tick = static_cast<std::uint32_t>(context.tick);
        snapshot_.last_input_sequence = session.last_input_sequence;
        snapshot_.entities.clear();
        for (const std::uint32_t index : nearby_) {
            snapshot_.entities.push_back(candidates_[index]);
        }

        writer_.clear();
        encode(writer_, snapshot_);
        transport_.send(peer, Channel::Unreliable, writer_.bytes());
    }

    transport_.flush();
}

} // namespace urt::net
