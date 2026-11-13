#include "urt/net/client.hpp"

#include "urt/core/log.hpp"
#include "urt/core/time.hpp"
#include "urt/ecs/components.hpp"
#include "urt/net/components.hpp"
#include "urt/net/interpolation.hpp"

#include <algorithm>
#include <utility>

namespace urt::net {
namespace {

// Commands repeated in every packet. With 4, three packets in a row can be
// lost before a command is.
constexpr std::size_t input_redundancy = 4;

// Unconfirmed commands kept for replay: about two seconds at 30 ticks/s.
constexpr std::size_t max_pending_inputs = 64;

// A correction larger than this is not hidden but shown at once: the server
// moved the player on purpose (a teleport), or we were badly out of step.
constexpr float snap_distance = 3.0f;

// How much of a hidden correction remains after each tick.
constexpr float correction_decay = 0.85f;

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
    wanted_.move_x = move_x;
    wanted_.move_z = move_z;
    wanted_.yaw = yaw;
}

bool Client::predicted_pose(float tick_fraction, Pose& out) const {
    if (!has_prediction_) {
        return false;
    }
    const float t = std::clamp(tick_fraction, 0.0f, 1.0f);
    out.position = drawn_previous_.position + (drawn_current_.position - drawn_previous_.position) * t;
    out.yaw = lerp_angle(drawn_previous_.yaw, drawn_current_.yaw, t);
    return true;
}

// Runs once per tick while connected: turns the wanted input into the next
// command, applies it to our own player immediately, and sends it.
void Client::send_input() {
    const MoveCommand command = sanitize_move(wanted_.move_x, wanted_.move_z, wanted_.yaw);
    ++sequence_;
    pending_.push_back(PendingInput{sequence_, command});
    if (pending_.size() > max_pending_inputs) {
        pending_.pop_front(); // the server has been silent for too long to replay everything
    }

    if (has_prediction_) {
        // Prediction: do now what the server will do when the command arrives.
        apply_move(predicted_.position, predicted_.yaw, command, walk_speed_, tick_seconds_);

        // What is drawn moves from the previous tick's pose to this tick's.
        // A hidden correction fades out a little every tick.
        correction_ *= correction_decay;
        if (correction_.x * correction_.x + correction_.y * correction_.y + correction_.z * correction_.z < 1e-6f) {
            correction_ = glm::vec3{0.0f};
        }
        drawn_previous_ = drawn_current_;
        drawn_current_ = Pose{.position = predicted_.position + correction_, .yaw = predicted_.yaw};
        if (snap_) {
            drawn_previous_ = drawn_current_; // do not glide across a teleport
            snap_ = false;
        }
    }

    // Send the newest few commands, oldest first.
    ClientInput packet;
    packet.sequence = sequence_;
    const std::size_t count = std::min(pending_.size(), input_redundancy);
    packet.count = static_cast<std::uint8_t>(count);
    for (std::size_t i = 0; i < count; ++i) {
        const MoveCommand& sent = pending_[pending_.size() - count + i].command;
        packet.commands[i] = InputCommand{.move_x = sent.move.x, .move_z = sent.move.y, .yaw = sent.yaw};
    }
    writer_.clear();
    encode(writer_, packet);
    transport_.send(server_peer_, Channel::Unreliable, writer_.bytes());
}

// Runs for every snapshot. The server says: "after your command number
// `confirmed_sequence`, you were here". Everything we sent after that is
// still on its way, so we start from the confirmed pose and apply those
// commands again. If the result equals what we predicted, nothing visible
// happens. If not (a lost command, or the server moved us), we are corrected.
void Client::reconcile(const SnapshotEntity& confirmed, std::uint32_t confirmed_sequence) {
    while (!pending_.empty() && !sequence_newer(pending_.front().sequence, confirmed_sequence)) {
        pending_.pop_front();
    }

    Pose replayed{.position = confirmed.position, .yaw = confirmed.yaw};
    for (const PendingInput& input : pending_) {
        apply_move(replayed.position, replayed.yaw, input.command, walk_speed_, tick_seconds_);
    }

    if (!has_prediction_) {
        // First snapshot: nothing to compare with yet.
        has_prediction_ = true;
        correction_ = glm::vec3{0.0f};
        drawn_previous_ = drawn_current_ = replayed;
    } else {
        // How far what we showed is from what is right.
        const glm::vec3 error = predicted_.position - replayed.position;
        const float distance_squared = error.x * error.x + error.y * error.y + error.z * error.z;
        if (distance_squared > snap_distance * snap_distance) {
            correction_ = glm::vec3{0.0f};
            snap_ = true;
        } else {
            // Keep drawing where we were and let the offset fade, instead of
            // jumping to the corrected position.
            correction_ += error;
        }
    }
    predicted_ = replayed;
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
        send_input();
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
        walk_speed_ = welcome.walk_speed;
        // The same rounding the server's own clock uses, so both sides
        // multiply by exactly the same number.
        tick_seconds_ = static_cast<float>(FixedTimestep(welcome.tick_rate).dt());
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
    const SnapshotEntity* own = nullptr;

    for (const SnapshotEntity& remote : snapshot.entities) {
        if (remote.id == player_id_) {
            own = &remote;
        }
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

    if (own != nullptr) {
        reconcile(*own, snapshot.last_input_sequence);
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

    pending_.clear();
    sequence_ = 0;
    has_prediction_ = false;
    correction_ = glm::vec3{0.0f};
    snap_ = false;
}

} // namespace urt::net
