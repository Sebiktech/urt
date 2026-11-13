#include "urt/net/server.hpp"

#include "urt/core/log.hpp"
#include "urt/ecs/components.hpp"
#include "urt/net/components.hpp"

#include <algorithm>
#include <cmath>

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
    config_.max_queued_inputs = std::max<std::size_t>(config_.max_queued_inputs, 1);
    config_.walk_speed = std::max(config_.walk_speed, 0.0f);
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

    consume_inputs(world);
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
        handle_input(session, data);
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

    session.active = true;
    session.entity = entity;
    session.player_id = id;
    session.name = std::move(hello.name);
    ++player_count_;

    writer_.clear();
    encode(writer_, ServerWelcome{.player_id = id,
                                  .tick_rate = tick_rate_of(context),
                                  .server_tick = static_cast<std::uint32_t>(context.tick),
                                  .walk_speed = config_.walk_speed});
    transport_.send(peer, Channel::Reliable, writer_.bytes());

    log::info("net", "'{}' joined as player {} ({} online)", session.name, id, player_count_);
}

void Server::handle_input(Session& session, std::span<const std::uint8_t> data) {
    ClientInput input;
    if (!decode(data, input)) {
        // Unreliable packets are never resent, so a damaged one is simply
        // skipped. The next packet repeats its commands.
        return;
    }

    // The packet holds `count` commands ending at `sequence`. Most of them
    // were already delivered by earlier packets; only take the new ones.
    for (std::size_t i = 0; i < input.count; ++i) {
        const std::uint32_t sequence = input.sequence - static_cast<std::uint32_t>(input.count - 1 - i);
        if (session.has_received && !sequence_newer(sequence, session.last_received_sequence)) {
            continue;
        }
        session.has_received = true;
        session.last_received_sequence = sequence;

        const InputCommand& raw = input.commands[i];
        session.queue.push_back(QueuedInput{sequence, sanitize_move(raw.move_x, raw.move_z, raw.yaw)});

        // A client that sends commands faster than the server ticks (a
        // speed hack, or a clock running fast) only fills its queue: the
        // overflow is thrown away, never simulated.
        if (session.queue.size() > config_.max_queued_inputs) {
            session.queue.pop_front();
        }
    }
}

// Exactly one command per player per tick. This is what ties a player's
// speed to the server's clock no matter how many commands the client sends,
// and what lets the client predict the result: the position after command N
// depends only on the commands up to N.
void Server::consume_inputs(World& world) {
    auto& registry = world.registry;
    for (auto& [peer, session] : sessions_) {
        if (!session.active) {
            continue;
        }
        Velocity& velocity = registry.get<Velocity>(session.entity);
        if (session.queue.empty()) {
            // Nothing arrived in time (lag, or the client stopped): stand
            // still. Guessing would make the server disagree with the
            // client's own prediction.
            velocity.linear = glm::vec3{0.0f};
            continue;
        }
        const QueuedInput input = session.queue.front();
        session.queue.pop_front();

        velocity.linear = move_velocity(input.command, config_.walk_speed);
        registry.get<Transform>(session.entity).yaw = input.command.yaw;
        session.last_input_sequence = input.sequence;
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
