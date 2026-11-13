#pragma once

// The client side of networking.
//
// Each tick the client reads what the server sent, mirrors the newest
// snapshot into the local world, and sends the player's current input.
// The client never moves entities by itself: every position comes from the
// server. (Prediction and interpolation will soften that later.)

#include "urt/ecs/system.hpp"
#include "urt/ecs/world.hpp"
#include "urt/net/protocol.hpp"
#include "urt/net/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace urt::net {

struct ClientConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = default_port;
    std::string name = "player";
};

enum class ClientState {
    Disconnected,
    Connecting,  // waiting for the transport connection
    Handshaking, // hello sent, waiting for welcome
    Connected,
};

class Client final : public System {
public:
    explicit Client(ClientConfig config);

    // Starts connecting. Returns false if the address cannot be used.
    // Progress shows in state() over the following ticks.
    bool connect();

    // Leaves the server and removes all replicated entities on the next tick.
    void disconnect();

    ClientState state() const { return state_; }

    // Why the server refused us, if it did.
    std::optional<RejectReason> reject_reason() const { return reject_reason_; }

    // Network id of our player. 0 until connected.
    NetId player_id() const { return player_id_; }

    // Our player's local entity. entt::null until the first snapshot arrives.
    Entity player_entity() const { return player_entity_; }

    // The local entity for a network id, or entt::null if we cannot see it.
    Entity find(NetId id) const;

    // How many replicated entities we currently see, our own included.
    std::size_t visible_count() const { return entities_.size(); }

    std::uint16_t server_tick_rate() const { return server_tick_rate_; }
    std::uint32_t last_snapshot_tick() const { return last_snapshot_tick_; }
    std::uint64_t snapshots_received() const { return snapshots_received_; }

    // What the player wants to do. Sent to the server every tick until changed.
    // move_x and move_z are a world-space direction, each in -1..1.
    void set_input(float move_x, float move_z, float yaw);

    std::string_view name() const override { return "net-client"; }
    void tick(World& world, const TickContext& context) override;

private:
    struct Replicated {
        Entity entity = entt::null;
        std::uint32_t seen_tick = 0; // server tick of the last snapshot containing it
    };

    void handle_packet(World& world, Channel channel, std::span<const std::uint8_t> data);
    void apply_snapshot(World& world, const ServerSnapshot& snapshot);
    void clear_world(World& world);

    ClientConfig config_;
    Transport transport_;
    ClientState state_ = ClientState::Disconnected;
    PeerId server_peer_ = 0;
    bool disconnect_requested_ = false;
    std::optional<RejectReason> reject_reason_;

    NetId player_id_ = 0;
    Entity player_entity_ = entt::null;
    std::uint16_t server_tick_rate_ = 0;
    std::uint32_t last_snapshot_tick_ = 0;
    bool has_snapshot_ = false;
    std::uint64_t snapshots_received_ = 0;
    std::unordered_map<NetId, Replicated> entities_;

    ClientInput input_;

    std::vector<Event> events_;
    ServerSnapshot snapshot_;
    ByteWriter writer_;
};

} // namespace urt::net
