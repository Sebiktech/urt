#pragma once

// The client side of networking.
//
// Each tick the client reads what the server sent, mirrors the newest
// snapshot into the local world, and sends the player's current input.
//
// Other entities only ever move as the server says. The client's own player
// is predicted: its input is applied locally at once, so walking feels
// immediate, and every snapshot is used to check that prediction (see
// predicted_pose and docs/network.md).

#include "urt/ecs/system.hpp"
#include "urt/ecs/world.hpp"
#include "urt/net/interpolation.hpp"
#include "urt/net/movement.hpp"
#include "urt/net/protocol.hpp"
#include "urt/net/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
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

// Something the server sent for the client's scripts, in arrival order.
struct ScriptMessage {
    enum class Kind {
        ResourceStart, // run `start`
        ResourceStop,  // stop the resource named `resource`
        Event,         // deliver `event` to the scripts
        Reset,         // the connection ended: stop everything
    };

    Kind kind = Kind::Reset;
    ServerResourceStart start;
    std::string resource;
    NetEvent event;
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

    // Server time of the newest snapshot, in seconds: its tick divided by the
    // server's tick rate. 0 before the first snapshot.
    double latest_server_time() const {
        return server_tick_rate_ != 0 ? static_cast<double>(last_snapshot_tick_) / server_tick_rate_ : 0.0;
    }

    // What the player wants to do. Sent to the server every tick until changed.
    // move_x and move_z are a world-space direction, each in -1..1.
    void set_input(float move_x, float move_z, float yaw);

    // Where to draw our own player. Unlike the Transform, which holds the
    // last position the server confirmed, this already includes the inputs
    // the server has not answered yet.
    // tick_fraction (0..1) is how far the frame is between the last tick and
    // the next; it blends between the two so movement is smooth at any frame
    // rate. Returns false until the first snapshot has arrived.
    bool predicted_pose(float tick_fraction, Pose& out) const;

    // Hands over what the server sent for scripts since the last call. The
    // client's script system calls this every tick.
    std::vector<ScriptMessage> take_script_messages();

    // Sends a script event to the server. Returns false when not connected.
    bool send_event(const NetEvent& event);

    // Input commands sent but not yet confirmed by the server.
    std::size_t pending_inputs() const { return pending_.size(); }

    // Length of one simulation tick as the server announced it, in seconds.
    float tick_seconds() const { return tick_seconds_; }

    std::string_view name() const override { return "net-client"; }
    void tick(World& world, const TickContext& context) override;

private:
    struct Replicated {
        Entity entity = entt::null;
        std::uint32_t seen_tick = 0; // server tick of the last snapshot containing it
    };

    struct PendingInput {
        std::uint32_t sequence = 0;
        MoveCommand command;
    };

    void handle_packet(World& world, Channel channel, std::span<const std::uint8_t> data);
    void apply_snapshot(World& world, const ServerSnapshot& snapshot);
    void reconcile(const SnapshotEntity& confirmed, std::uint32_t confirmed_sequence);
    void send_input();
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

    // Prediction of our own player.
    InputCommand wanted_;              // what set_input() last asked for
    std::uint32_t sequence_ = 0;       // sequence number of the newest command
    std::deque<PendingInput> pending_; // sent, not yet confirmed; oldest first
    float walk_speed_ = 0.0f;
    float tick_seconds_ = 0.0f;
    bool has_prediction_ = false;
    Pose predicted_;                   // confirmed position plus all pending commands
    glm::vec3 correction_{0.0f};       // drawn offset that hides small corrections, fading out
    bool snap_ = false;                // the last correction was too big to hide
    Pose drawn_previous_;              // what was drawn at the previous tick
    Pose drawn_current_;               // what is drawn at this tick

    std::vector<ScriptMessage> script_messages_;

    std::vector<Event> events_;
    ServerSnapshot snapshot_;
    ByteWriter writer_;
};

} // namespace urt::net
