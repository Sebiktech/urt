// End-to-end networking tests: a real server and real clients in one process,
// talking over UDP on the loopback interface.

#include <doctest/doctest.h>

#include "urt/app/application.hpp"
#include "urt/core/log.hpp"
#include "urt/ecs/components.hpp"
#include "urt/ecs/movement_system.hpp"
#include "urt/net/client.hpp"
#include "urt/net/components.hpp"
#include "urt/net/interpolation.hpp"
#include "urt/net/server.hpp"
#include "urt/net/transport.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace urt;
using namespace urt::net;

namespace {

constexpr double tick_rate = 50.0; // dt = 0.02 s

AppConfig app_config() {
    AppConfig config;
    config.tick_rate = tick_rate;
    config.stats_interval_seconds = 0.0;
    config.worker_threads = 1;
    return config;
}

// Silences expected "dropping peer" warnings for the duration of a test.
struct QuietLog {
    log::Level previous = log::level();
    QuietLog() { log::set_level(log::Level::Off); }
    ~QuietLog() { log::set_level(previous); }
};

struct TestServer {
    Application app{app_config()};
    Server& server;

    explicit TestServer(ServerConfig config = {})
        : server(app.add_system<Server>([&] {
              config.port = 0; // let the OS pick a free port
              return config;
          }())) {
        app.add_system<MovementSystem>();
        app.add_system<ServerSnapshotSystem>(server);
        REQUIRE(server.start());
        REQUIRE(server.port() != 0);
    }
};

struct TestClient {
    Application app{app_config()};
    Client& client;

    TestClient(const TestServer& server, std::string name)
        : client(app.add_system<Client>(
              ClientConfig{.host = "127.0.0.1", .port = server.server.port(), .name = std::move(name)})) {
        REQUIRE(client.connect());
    }

    const Transform* transform_of(NetId id) {
        const Entity entity = client.find(id);
        return entity == entt::null ? nullptr : &app.world().registry.get<Transform>(entity);
    }
};

// Runs everyone tick by tick. The short sleep gives the OS time to deliver
// the UDP packets between the sockets.
struct Session {
    TestServer& server;
    std::vector<TestClient*> clients;

    void pump(int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            server.app.step();
            for (TestClient* client : clients) {
                client->app.step();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Pumps until the condition holds. Returns false after ~3000 ticks.
    bool pump_until(const std::function<bool()>& condition) {
        for (int i = 0; i < 3000; ++i) {
            if (condition()) {
                return true;
            }
            pump();
        }
        return condition();
    }
};

// A hand-driven player: it speaks the protocol directly, so a test can send
// exactly the packets it wants, including ones a real client never would.
struct RawPlayer {
    Transport transport;
    PeerId server_peer = 0;
    NetId player_id = 0; // set once the server has welcomed us
    std::vector<Event> events;
    ByteWriter writer;

    explicit RawPlayer(std::uint16_t port) { server_peer = transport.connect("127.0.0.1", port); }

    void poll() {
        events.clear();
        transport.poll(events);
        for (const Event& event : events) {
            if (event.type == Event::Type::Connected) {
                writer.clear();
                encode(writer, ClientHello{.version = protocol_version, .name = "Raw"});
                transport.send(server_peer, Channel::Reliable, writer.bytes());
            } else if (event.type == Event::Type::Received) {
                ServerWelcome welcome;
                if (decode(event.data, welcome)) {
                    player_id = welcome.player_id;
                }
            }
        }
        transport.flush();
    }

    // Sends one packet whose commands all walk in +x and end at `newest_sequence`.
    void send_walk(std::uint32_t newest_sequence, std::uint8_t count) {
        ClientInput packet{.sequence = newest_sequence, .count = count};
        for (std::uint8_t i = 0; i < count; ++i) {
            packet.commands[i].move_x = 1.0f;
        }
        writer.clear();
        encode(writer, packet);
        transport.send(server_peer, Channel::Unreliable, writer.bytes());
        transport.flush();
    }
};

// doctest takes `a == b` apart to print both sides, and that collides with
// EnTT's templated comparison against entt::null. Hence this helper.
bool exists(Entity entity) { return entity != entt::null; }

bool connected(const TestClient& client) {
    return client.client.state() == ClientState::Connected &&
           client.client.player_entity() != entt::null;
}

} // namespace

TEST_CASE("two clients connect, see each other and move with server authority") {
    QuietLog quiet;
    TestServer server;
    TestClient anna(server, "Anna");
    TestClient boris(server, "Boris");
    Session session{server, {&anna, &boris}};

    REQUIRE(session.pump_until([&] { return connected(anna) && connected(boris); }));
    CHECK(server.server.player_count() == 2);
    CHECK(anna.client.player_id() != boris.client.player_id());
    CHECK(anna.client.server_tick_rate() == 50);

    // Each sees both players.
    REQUIRE(session.pump_until(
        [&] { return anna.client.visible_count() == 2 && boris.client.visible_count() == 2; }));
    CHECK(anna.app.world().registry.all_of<LocalPlayer>(anna.client.player_entity()));
    CHECK_FALSE(anna.app.world().registry.all_of<LocalPlayer>(anna.client.find(boris.client.player_id())));

    const Entity anna_on_server = server.server.player_entity(anna.client.player_id());
    REQUIRE(exists(anna_on_server));
    auto& server_registry = server.app.world().registry;
    const float start_x = server_registry.get<Transform>(anna_on_server).position.x;

    // Anna asks to move five times faster than allowed. The server clamps the
    // direction to length 1 and applies its own speed of 4 m/s.
    anna.client.set_input(5.0f, 0.0f, 1.0f);
    session.pump(50); // one simulated second

    const Transform& truth = server_registry.get<Transform>(anna_on_server);
    const float moved = truth.position.x - start_x;
    CHECK(moved > 3.0f);  // a few ticks pass before the first input arrives
    CHECK(moved <= 4.0f + 1e-3f);
    CHECK(server_registry.get<Velocity>(anna_on_server).linear.x == doctest::Approx(4.0f));
    CHECK(truth.yaw == doctest::Approx(1.0f));

    // Boris sees Anna where the server says she is, give or take a few ticks
    // of travel (4 m/s * 0.02 s = 0.08 m per tick).
    const Transform* seen = boris.transform_of(anna.client.player_id());
    REQUIRE(seen != nullptr);
    CHECK(std::abs(seen->position.x - truth.position.x) < 0.5f);
    CHECK(seen->yaw == doctest::Approx(1.0f));

    // The client also keeps where Anna was a moment ago, for smooth drawing:
    // 0.1 s back she was about 0.4 m (4 m/s) behind where she is now.
    const auto& history = boris.app.world().registry.get<PoseHistory>(boris.client.find(anna.client.player_id()));
    CHECK(history.size() == PoseHistory::capacity);
    Pose now_pose;
    Pose earlier_pose;
    REQUIRE(history.sample(boris.client.latest_server_time(), now_pose));
    REQUIRE(history.sample(boris.client.latest_server_time() - 0.1, earlier_pose));
    CHECK(now_pose.position.x == seen->position.x);
    CHECK(now_pose.position.x - earlier_pose.position.x == doctest::Approx(0.4f).epsilon(0.05));

    // Stop, let everything settle, then the views must match exactly.
    anna.client.set_input(0.0f, 0.0f, 1.0f);
    session.pump(20);
    seen = boris.transform_of(anna.client.player_id());
    REQUIRE(seen != nullptr);
    CHECK(seen->position.x == truth.position.x);
    CHECK(anna.transform_of(anna.client.player_id())->position.x == truth.position.x);
}

TEST_CASE("a leaving player disappears for everyone") {
    QuietLog quiet;
    TestServer server;
    TestClient anna(server, "Anna");
    TestClient boris(server, "Boris");
    Session session{server, {&anna, &boris}};
    REQUIRE(session.pump_until([&] { return anna.client.visible_count() == 2 && boris.client.visible_count() == 2; }));

    const NetId anna_id = anna.client.player_id();
    anna.client.disconnect();

    REQUIRE(session.pump_until([&] { return server.server.player_count() == 1; }));
    REQUIRE(session.pump_until([&] { return boris.client.visible_count() == 1; }));
    CHECK_FALSE(exists(boris.client.find(anna_id)));
    CHECK_FALSE(exists(server.server.player_entity(anna_id)));
    CHECK(server.app.world().entity_count() == 1);

    CHECK(anna.client.state() == ClientState::Disconnected);
    CHECK(anna.client.visible_count() == 0);
    CHECK(anna.app.world().entity_count() == 0);
}

TEST_CASE("clients only receive entities within the interest radius") {
    QuietLog quiet;
    ServerConfig config;
    config.interest_radius = 50.0f;
    TestServer server(config);
    TestClient anna(server, "Anna");
    TestClient boris(server, "Boris");
    Session session{server, {&anna, &boris}};

    // A networked NPC near the spawn area and one far away.
    auto& registry = server.app.world().registry;
    const Entity near_npc = registry.create();
    registry.emplace<Transform>(near_npc, Transform{.position = {10.0f, 0.0f, 0.0f}});
    const NetId near_id = server.server.make_networked(server.app.world(), near_npc);
    const Entity far_npc = registry.create();
    registry.emplace<Transform>(far_npc, Transform{.position = {1000.0f, 0.0f, 0.0f}});
    const NetId far_id = server.server.make_networked(server.app.world(), far_npc);

    REQUIRE(session.pump_until([&] { return anna.client.visible_count() == 3 && boris.client.visible_count() == 3; }));
    CHECK(exists(anna.client.find(near_id)));
    CHECK_FALSE(exists(anna.client.find(far_id)));

    // Move Anna next to the far NPC (as a teleport script would).
    const Entity anna_on_server = server.server.player_entity(anna.client.player_id());
    registry.get<Transform>(anna_on_server).position = {1010.0f, 0.0f, 0.0f};

    REQUIRE(session.pump_until([&] { return anna.client.find(far_id) != entt::null; }));
    CHECK(anna.client.visible_count() == 2); // herself and the far NPC
    CHECK_FALSE(exists(anna.client.find(near_id)));
    CHECK_FALSE(exists(anna.client.find(boris.client.player_id())));
    CHECK(exists(anna.client.player_entity()));

    REQUIRE(session.pump_until([&] { return boris.client.visible_count() == 2; }));
    CHECK_FALSE(exists(boris.client.find(anna.client.player_id())));
    CHECK(exists(boris.client.find(near_id)));
}

TEST_CASE("the server refuses bad names and extra players") {
    QuietLog quiet;
    ServerConfig config;
    config.max_players = 1;
    TestServer server(config);

    TestClient bad(server, "evil\nname"); // a line break would forge log lines
    Session session{server, {&bad}};
    REQUIRE(session.pump_until([&] { return bad.client.state() == ClientState::Disconnected; }));
    CHECK(bad.client.reject_reason() == RejectReason::BadName);
    CHECK(server.server.player_count() == 0);

    TestClient first(server, "First");
    session.clients = {&first};
    REQUIRE(session.pump_until([&] { return connected(first); }));

    TestClient second(server, "Second");
    session.clients = {&first, &second};
    REQUIRE(session.pump_until([&] { return second.client.state() == ClientState::Disconnected; }));
    CHECK(second.client.reject_reason() == RejectReason::ServerFull);
    CHECK(server.server.player_count() == 1);
    CHECK(first.client.state() == ClientState::Connected);
}

TEST_CASE("the server drops misbehaving connections and keeps serving the rest") {
    QuietLog quiet;
    ServerConfig config;
    config.hello_timeout_seconds = 0.4; // 20 ticks
    TestServer server(config);
    TestClient good(server, "Good");
    Session session{server, {&good}};
    REQUIRE(session.pump_until([&] { return connected(good); }));

    // A raw connection that speaks the transport but not the protocol.
    struct RawPeer {
        Transport transport;
        PeerId server_peer = 0;
        bool is_connected = false;
        bool was_dropped = false;
        std::vector<Event> events;

        explicit RawPeer(std::uint16_t port) { server_peer = transport.connect("127.0.0.1", port); }

        void poll() {
            events.clear();
            transport.poll(events);
            for (const Event& event : events) {
                is_connected = is_connected || event.type == Event::Type::Connected;
                was_dropped = was_dropped || event.type == Event::Type::Disconnected;
            }
        }
    };

    auto run_raw = [&](const std::function<void(RawPeer&)>& misbehave) {
        RawPeer raw(server.server.port());
        REQUIRE(raw.server_peer != 0);
        REQUIRE(session.pump_until([&] {
            raw.poll();
            return raw.is_connected;
        }));
        misbehave(raw);
        raw.transport.flush();
        return session.pump_until([&] {
            raw.poll();
            return raw.was_dropped;
        });
    };

    ByteWriter writer;

    SUBCASE("garbage instead of a hello") {
        const std::vector<std::uint8_t> garbage{0xFF, 0x00, 0x13, 0x37};
        CHECK(run_raw([&](RawPeer& raw) { raw.transport.send(raw.server_peer, Channel::Reliable, garbage); }));
    }
    SUBCASE("input before the handshake") {
        encode(writer, ClientInput{.sequence = 1, .count = 1});
        CHECK(run_raw([&](RawPeer& raw) {
            raw.transport.send(raw.server_peer, Channel::Unreliable, writer.bytes());
        }));
    }
    SUBCASE("hello on the wrong channel") {
        encode(writer, ClientHello{.version = protocol_version, .name = "Sneaky"});
        CHECK(run_raw([&](RawPeer& raw) {
            raw.transport.send(raw.server_peer, Channel::Unreliable, writer.bytes());
        }));
    }
    SUBCASE("a second hello after joining") {
        encode(writer, ClientHello{.version = protocol_version, .name = "Twice"});
        CHECK(run_raw([&](RawPeer& raw) {
            raw.transport.send(raw.server_peer, Channel::Reliable, writer.bytes());
            raw.transport.send(raw.server_peer, Channel::Reliable, writer.bytes());
        }));
    }
    SUBCASE("a wrong protocol version gets a reject") {
        writer.u8(static_cast<std::uint8_t>(MessageType::ClientHello));
        writer.u16(static_cast<std::uint16_t>(protocol_version + 1));
        CHECK(run_raw([&](RawPeer& raw) {
            raw.transport.send(raw.server_peer, Channel::Reliable, writer.bytes());
        }));
    }
    SUBCASE("silence after connecting") {
        CHECK(run_raw([](RawPeer&) {}));
    }

    // Whatever the raw peer did, the real player is unaffected.
    CHECK(server.server.player_count() == 1);
    const std::uint64_t before = good.client.snapshots_received();
    session.pump(10);
    CHECK(good.client.state() == ClientState::Connected);
    CHECK(good.client.snapshots_received() > before);
}

TEST_CASE("a player whose inputs stop arriving stops moving") {
    QuietLog quiet;
    TestServer server;
    TestClient anna(server, "Anna");
    Session session{server, {&anna}};
    REQUIRE(session.pump_until([&] { return connected(anna); }));

    anna.client.set_input(0.0f, 1.0f, 0.0f);
    session.pump(10);
    const Entity on_server = server.server.player_entity(anna.client.player_id());
    auto& registry = server.app.world().registry;
    CHECK(registry.get<Velocity>(on_server).linear.z == doctest::Approx(4.0f));

    // The client freezes: only the server keeps ticking. It uses up the few
    // commands it still has and then holds the player still.
    for (int i = 0; i < 20; ++i) {
        server.app.step();
    }
    CHECK(registry.get<Velocity>(on_server).linear.z == 0.0f);
}

TEST_CASE("prediction moves the player at once and matches the server exactly") {
    QuietLog quiet;
    TestServer server;
    TestClient anna(server, "Anna");
    Session session{server, {&anna}};
    REQUIRE(session.pump_until([&] { return connected(anna); }));
    session.pump(5);

    const Entity on_server = server.server.player_entity(anna.client.player_id());
    auto& server_registry = server.app.world().registry;
    const Transform& truth = server_registry.get<Transform>(on_server);
    const Transform& confirmed = anna.app.world().registry.get<Transform>(anna.client.player_entity());

    Pose predicted;
    REQUIRE(anna.client.predicted_pose(1.0f, predicted));
    const float start_x = predicted.position.x;
    CHECK(start_x == truth.position.x);

    anna.client.set_input(1.0f, 0.0f, 0.5f);

    // The very first tick already moves the predicted player by one step
    // (4 m/s * 0.02 s), before the server has even received the command.
    anna.app.step();
    REQUIRE(anna.client.predicted_pose(1.0f, predicted));
    CHECK(predicted.position.x - start_x == doctest::Approx(0.08f));
    CHECK(predicted.yaw == doctest::Approx(0.5f));
    CHECK(confirmed.position.x == start_x); // the server has confirmed nothing yet
    server.app.step();

    // While walking, every tick moves the drawn player by exactly one step.
    // Any disagreement with the server would show up here as an uneven step.
    float previous_x = predicted.position.x;
    for (int i = 0; i < 60; ++i) {
        session.pump();
        REQUIRE(anna.client.predicted_pose(1.0f, predicted));
        CHECK(predicted.position.x - previous_x == doctest::Approx(0.08f).epsilon(0.001));
        previous_x = predicted.position.x;
        // Prediction is ahead of what the server has confirmed, by the
        // commands still on their way.
        CHECK(predicted.position.x >= confirmed.position.x);
        CHECK(anna.client.pending_inputs() <= 6);
    }

    // Halfway between two ticks the drawn pose is halfway between them.
    Pose half;
    REQUIRE(anna.client.predicted_pose(0.5f, half));
    CHECK(half.position.x == doctest::Approx(predicted.position.x - 0.04f));

    // Stop and let the last commands arrive: prediction and server agree to
    // the last bit, because both did the same arithmetic.
    anna.client.set_input(0.0f, 0.0f, 0.5f);
    session.pump(20);
    REQUIRE(anna.client.predicted_pose(1.0f, predicted));
    CHECK(predicted.position.x == truth.position.x);
    CHECK(predicted.position.z == truth.position.z);
    CHECK(confirmed.position.x == truth.position.x);
    CHECK(truth.position.x - start_x == doctest::Approx(61 * 0.08f).epsilon(0.001));
}

TEST_CASE("sending inputs faster than the server ticks does not make a player faster") {
    QuietLog quiet;
    TestServer server;
    Session session{server, {}};
    RawPlayer cheat(server.server.port());
    REQUIRE(session.pump_until([&] {
        cheat.poll();
        return cheat.player_id != 0;
    }));

    const Entity on_server = server.server.player_entity(cheat.player_id);
    REQUIRE(exists(on_server));
    const Transform& truth = server.app.world().registry.get<Transform>(on_server);
    const float start_x = truth.position.x;

    // Five new commands every tick: five times the honest rate.
    std::uint32_t sequence = 0;
    constexpr int ticks = 100;
    for (int i = 0; i < ticks; ++i) {
        sequence += 5;
        cheat.send_walk(sequence, 5);
        cheat.poll();
        session.pump();
    }

    // One command is simulated per tick, so the distance is at most what an
    // honest player covers: 100 ticks * 0.08 m.
    const float moved = truth.position.x - start_x;
    CHECK(moved > ticks * 0.08f * 0.9f);
    CHECK(moved <= ticks * 0.08f + 1e-3f);
    CHECK(server.server.player_count() == 1);
}

TEST_CASE("commands from lost packets are recovered from the next packet") {
    QuietLog quiet;
    TestServer server;
    Session session{server, {}};
    RawPlayer player(server.server.port());
    REQUIRE(session.pump_until([&] {
        player.poll();
        return player.player_id != 0;
    }));
    const Transform& truth =
        server.app.world().registry.get<Transform>(server.server.player_entity(player.player_id));
    const float start_x = truth.position.x;

    // Command 1 arrives. The packets for commands 2 and 3 are "lost" (never
    // sent). The packet for command 4 repeats 1 to 4.
    player.send_walk(1, 1);
    session.pump(3);
    player.send_walk(4, 4);
    session.pump(10);
    CHECK(truth.position.x - start_x == doctest::Approx(4 * 0.08f).epsilon(0.001));

    // The same packet again (a duplicate on the network) changes nothing,
    // and neither does an old one arriving late.
    player.send_walk(4, 4);
    player.send_walk(2, 2);
    session.pump(10);
    CHECK(truth.position.x - start_x == doctest::Approx(4 * 0.08f).epsilon(0.001));
}

TEST_CASE("server corrections are hidden when small and shown at once when large") {
    QuietLog quiet;
    TestServer server;
    TestClient anna(server, "Anna");
    Session session{server, {&anna}};
    REQUIRE(session.pump_until([&] { return connected(anna); }));
    session.pump(5);

    Transform& truth = server.app.world().registry.get<Transform>(server.server.player_entity(anna.client.player_id()));
    Pose drawn;
    REQUIRE(anna.client.predicted_pose(1.0f, drawn));

    SUBCASE("a nudge of one metre fades in") {
        truth.position.x += 1.0f; // as a script or a collision would
        float previous_x = drawn.position.x;
        float largest_step = 0.0f;
        for (int i = 0; i < 120; ++i) {
            session.pump();
            REQUIRE(anna.client.predicted_pose(1.0f, drawn));
            largest_step = std::max(largest_step, std::abs(drawn.position.x - previous_x));
            previous_x = drawn.position.x;
        }
        CHECK(largest_step < 0.2f);                  // never a visible jump...
        CHECK(drawn.position.x == truth.position.x); // ...yet it ends up exactly right
    }

    SUBCASE("a teleport is shown immediately") {
        truth.position.x += 1000.0f;
        const float before = drawn.position.x;
        REQUIRE(session.pump_until([&] {
            anna.client.predicted_pose(1.0f, drawn);
            return drawn.position.x != before;
        }));
        CHECK(drawn.position.x == truth.position.x); // one step, no gliding
        Pose between;
        REQUIRE(anna.client.predicted_pose(0.5f, between));
        CHECK(between.position.x == truth.position.x); // not blended across the jump either
    }
}

TEST_CASE("Transport refuses an empty host name and a port that is taken") {
    QuietLog quiet;

    // Checked by Transport itself, because resolvers disagree: Linux rejects
    // an empty name, Windows resolves it to the local machine.
    Transport transport;
    CHECK(transport.connect("", 1) == 0);
    CHECK_FALSE(transport.is_open());
    CHECK_FALSE(transport.send(1, Channel::Reliable, {}));

    Transport first;
    REQUIRE(first.listen(0, 4));
    Transport second;
    CHECK_FALSE(second.listen(first.port(), 4));
}
