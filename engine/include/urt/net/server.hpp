#pragma once

// The server side of networking.
//
// Each tick has two network phases with the simulation in between:
//
//   Server::tick            read packets, accept players, take one input per player
//   MovementSystem, ...     simulate
//   ServerSnapshotSystem    send every client the entities near its player
//
// Add the systems to the Application in that order.

#include "urt/ecs/components.hpp"
#include "urt/ecs/system.hpp"
#include "urt/ecs/world.hpp"
#include "urt/net/interest_grid.hpp"
#include "urt/net/movement.hpp"
#include "urt/net/protocol.hpp"
#include "urt/net/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace urt::net {

struct ServerConfig {
    std::uint16_t port = default_port;
    std::size_t max_players = 64;
    float walk_speed = 4.0f;             // metres per second
    float interest_radius = 150.0f;      // metres; clients see entities this close
    std::uint32_t snapshot_interval = 1; // send a snapshot every N ticks
    double hello_timeout_seconds = 5.0;  // drop connections that never introduce themselves
    std::size_t max_queued_inputs = 8;   // inputs kept per player; more than that and the oldest is dropped

    // Script events a client may send. Each event uses one token; tokens
    // refill at `event_rate_per_second` up to `event_burst`. Events beyond
    // that are dropped, and a client that keeps flooding is disconnected.
    double event_rate_per_second = 30.0;
    double event_burst = 60.0;
    std::uint32_t max_dropped_events = 300;
};

// Something that happened to a player during the current tick.
struct PlayerEvent {
    enum class Kind { Joined, Left };

    Kind kind = Kind::Joined;
    NetId id = 0;
    std::string name;
    glm::vec3 position{0.0f}; // where the player appeared, or was last
};

// A script event a client sent during the current tick.
struct ReceivedEvent {
    NetId player = 0;
    NetEvent event;
};

struct PlayerInfo {
    NetId id = 0;
    std::string name;
    Entity entity = entt::null;
};

class Server final : public System {
public:
    explicit Server(ServerConfig config);

    // Opens the port. Returns false if that fails.
    bool start();

    // The port in use (useful when the config asked for port 0).
    std::uint16_t port() const { return transport_.port(); }

    // Players that completed the handshake.
    std::size_t player_count() const { return player_count_; }

    // Gives a server-side entity a network id, which makes clients see it.
    // The entity needs a Transform. Players get theirs automatically.
    NetId make_networked(World& world, Entity entity);

    // The entity a player controls, or entt::null.
    Entity player_entity(NetId player_id) const;

    bool is_player(NetId id) const { return player_entity(id) != entt::null; }

    // Everyone who is in the game right now.
    std::vector<PlayerInfo> players() const;

    // Players who joined or left during the current tick, in order. Filled
    // by tick(); systems that run after the server read it.
    const std::vector<PlayerEvent>& player_events() const { return player_events_; }

    // Script events clients sent during the current tick, in order.
    const std::vector<ReceivedEvent>& received_events() const { return received_events_; }

    // Sends an already encoded message on the reliable channel. Returns
    // false if there is no such player.
    bool send_reliable(NetId player, std::span<const std::uint8_t> message);

    // The same to every player in the game.
    void broadcast_reliable(std::span<const std::uint8_t> message);

    // Creates an entity clients can see, standing still at `transform`.
    NetId spawn(World& world, const Transform& transform);

    // Removes an entity created with spawn() or make_networked(). Players
    // cannot be removed this way. Returns false if there is no such entity.
    bool despawn(World& world, NetId id);

    // The entity with this network id, player or not, or entt::null.
    Entity find(World& world, NetId id);

    std::string_view name() const override { return "net-server"; }

    // Receive phase.
    void tick(World& world, const TickContext& context) override;

    // Send phase. Called by ServerSnapshotSystem.
    void send_snapshots(World& world, const TickContext& context);

private:
    struct QueuedInput {
        std::uint32_t sequence = 0;
        MoveCommand command;
    };

    struct Session {
        bool active = false; // handshake completed
        bool kicked = false; // disconnect requested, ignore whatever still arrives
        Entity entity = entt::null;
        NetId player_id = 0;
        std::string name;
        // Commands that arrived but have not been simulated yet, oldest first.
        std::deque<QueuedInput> queue;
        std::uint32_t last_received_sequence = 0;
        bool has_received = false;
        std::uint32_t last_input_sequence = 0; // last command simulated; reported in snapshots
        double event_tokens = 0.0;             // script events this client may still send
        std::uint32_t dropped_events = 0;
        std::uint64_t connected_tick = 0;
    };

    void handle_packet(World& world, const TickContext& context, PeerId peer, Session& session,
                       Channel channel, std::span<const std::uint8_t> data);
    void handle_hello(World& world, const TickContext& context, PeerId peer, Session& session,
                      std::span<const std::uint8_t> data);
    void handle_input(Session& session, std::span<const std::uint8_t> data);
    void handle_event(World& world, PeerId peer, Session& session, std::span<const std::uint8_t> data);
    void remove_session(World& world, PeerId peer);
    void reject(PeerId peer, Session& session, RejectReason reason);
    void kick(World& world, PeerId peer, Session& session, std::string_view why);
    void remove_player(World& world, Session& session);
    void consume_inputs(World& world);

    ServerConfig config_;
    Transport transport_;
    std::unordered_map<PeerId, Session> sessions_;
    std::unordered_map<NetId, Entity> networked_; // every entity with a network id
    std::vector<PlayerEvent> player_events_;
    std::vector<ReceivedEvent> received_events_;
    std::size_t player_count_ = 0;
    NetId next_net_id_ = 1;
    bool warned_snapshot_overflow_ = false;

    // Scratch buffers kept between ticks so they do not reallocate every time.
    std::vector<Event> events_;
    InterestGrid grid_;
    std::vector<SnapshotEntity> candidates_;
    std::vector<std::uint32_t> nearby_;
    ServerSnapshot snapshot_;
    ByteWriter writer_;
};

// Runs Server::send_snapshots. Add it after the systems that move things.
class ServerSnapshotSystem final : public System {
public:
    explicit ServerSnapshotSystem(Server& server) : server_(server) {}

    std::string_view name() const override { return "net-snapshots"; }
    void tick(World& world, const TickContext& context) override {
        server_.send_snapshots(world, context);
    }

private:
    Server& server_;
};

} // namespace urt::net
