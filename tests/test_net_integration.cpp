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
        encode(writer, ClientInput{.sequence = 1, .move_x = 1.0f});
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
    ServerConfig config;
    config.input_timeout_seconds = 0.2; // 10 ticks
    TestServer server(config);
    TestClient anna(server, "Anna");
    Session session{server, {&anna}};
    REQUIRE(session.pump_until([&] { return connected(anna); }));

    anna.client.set_input(0.0f, 1.0f, 0.0f);
    session.pump(10);
    const Entity on_server = server.server.player_entity(anna.client.player_id());
    auto& registry = server.app.world().registry;
    CHECK(registry.get<Velocity>(on_server).linear.z == doctest::Approx(4.0f));

    // The client freezes: only the server keeps ticking.
    for (int i = 0; i < 20; ++i) {
        server.app.step();
    }
    CHECK(registry.get<Velocity>(on_server).linear.z == 0.0f);
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
