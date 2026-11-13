// Scripting across the network: client scripts sent by the server, and events
// between client and server scripts. Real server, real clients, loopback UDP.

#include <doctest/doctest.h>

#include "urt/app/application.hpp"
#include "urt/core/log.hpp"
#include "urt/ecs/components.hpp"
#include "urt/ecs/movement_system.hpp"
#include "urt/net/client.hpp"
#include "urt/net/components.hpp"
#include "urt/net/server.hpp"
#include "urt/net/transport.hpp"
#include "urt/script/client_script_system.hpp"
#include "urt/script/script_system.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace urt;
using urt::script::ResourceState;

namespace {

struct LogCapture {
    std::vector<std::string> lines;
    log::Level previous = log::level();

    LogCapture() {
        log::set_level(log::Level::Info);
        log::set_sink([this](log::Level, std::string_view channel, std::string_view message) {
            lines.push_back(std::string(channel) + ": " + std::string(message));
        });
    }
    ~LogCapture() {
        log::set_sink({});
        log::set_level(previous);
    }

    int count(std::string_view text) const {
        return static_cast<int>(std::count_if(lines.begin(), lines.end(), [&](const std::string& line) {
            return line.find(text) != std::string::npos;
        }));
    }
    bool has(std::string_view text) const { return count(text) > 0; }
};

struct TempResources {
    fs::path root;

    TempResources() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() /
               ("urt-nettest-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(counter.fetch_add(1)));
        fs::create_directories(root);
    }
    ~TempResources() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    fs::path write(const std::string& resource, const std::string& file, const std::string& content) const {
        const fs::path path = root / resource / file;
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << content;
        return path;
    }

    // A resource with a server script and a client script.
    fs::path resource(const std::string& name, const std::string& server, const std::string& client) const {
        write(name, "resource.lua", "return { server = { 'server.lua' }, client = { 'client.lua' } }\n");
        write(name, "server.lua", server);
        write(name, "client.lua", client);
        return root / name;
    }
};

void touch(const fs::path& path, int seconds_ahead) {
    fs::last_write_time(path, fs::file_time_type::clock::now() + std::chrono::seconds(seconds_ahead));
}

AppConfig app_config() {
    AppConfig config;
    config.tick_rate = 50.0;
    config.stats_interval_seconds = 0.0;
    config.worker_threads = 1;
    return config;
}

struct TestServer {
    Application app{app_config()};
    net::Server& server;
    script::ScriptSystem& scripts;

    explicit TestServer(script::ScriptConfig script_config = {}, net::ServerConfig net_config = {})
        : server(app.add_system<net::Server>([&] {
              net_config.port = 0;
              return net_config;
          }())),
          scripts(app.add_system<script::ScriptSystem>(server, [&] {
              script_config.slow_call_seconds = 10.0;
              return script_config;
          }())) {
        app.add_system<MovementSystem>();
        app.add_system<net::ServerSnapshotSystem>(server);
        REQUIRE(server.start());
    }

    bool load(const fs::path& directory) { return scripts.load(app.world(), directory); }
};

struct TestClient {
    Application app{app_config()};
    net::Client& client;
    script::ClientScriptSystem& scripts;

    TestClient(const TestServer& server, std::string name, script::ClientScriptConfig config = {})
        : client(app.add_system<net::Client>(
              net::ClientConfig{.host = "127.0.0.1", .port = server.server.port(), .name = std::move(name)})),
          scripts(app.add_system<script::ClientScriptSystem>(client, [&] {
              config.slow_call_seconds = 10.0;
              return config;
          }())) {
        REQUIRE(client.connect());
    }
};

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

script::ScriptConfig manual_config() {
    script::ScriptConfig config;
    config.hot_reload = false;
    return config;
}

// A hand-driven client that speaks the protocol directly.
struct RawPlayer {
    net::Transport transport;
    net::PeerId server_peer = 0;
    net::NetId player_id = 0;
    bool dropped = false;
    bool say_hello = true;
    std::vector<net::Event> events;
    net::ByteWriter writer;

    explicit RawPlayer(std::uint16_t port) { server_peer = transport.connect("127.0.0.1", port); }

    void poll() {
        events.clear();
        transport.poll(events);
        for (const net::Event& event : events) {
            if (event.type == net::Event::Type::Connected && say_hello) {
                writer.clear();
                encode(writer, net::ClientHello{.version = net::protocol_version, .name = "Raw"});
                transport.send(server_peer, net::Channel::Reliable, writer.bytes());
            } else if (event.type == net::Event::Type::Received) {
                net::ServerWelcome welcome;
                if (decode(event.data, welcome)) {
                    player_id = welcome.player_id;
                }
            } else if (event.type == net::Event::Type::Disconnected) {
                dropped = true;
            }
        }
        transport.flush();
    }

    void send(std::span<const std::uint8_t> packet) {
        transport.send(server_peer, net::Channel::Reliable, packet);
        transport.flush();
    }

    void send_event(const std::string& name, std::vector<std::uint8_t> arguments) {
        writer.clear();
        encode(writer, net::NetEvent{name, std::move(arguments)});
        send(writer.bytes());
    }
};

} // namespace

TEST_CASE("client scripts arrive on join and exchange events with the server") {
    LogCapture log;
    TempResources files;
    TestServer server(manual_config());

    REQUIRE(server.load(files.resource("chat", R"(
        urt.onNet("chat:say", function(player, text, extra)
            urt.log("server got", player.name, text, extra.n, extra.list[2], extra.flag, extra.missing)
            urt.emitClient(player.id, "chat:echo", text:upper(), { answer = 42 })
            urt.emitAllClients("chat:broadcast", player.name)
        end)
    )", R"(
        urt.log("client script runs as player", urt.localPlayer())
        urt.onNet("chat:echo", function(text, data)
            urt.log("client got echo", text, data.answer)
        end)
        urt.onNet("chat:broadcast", function(name)
            urt.log("broadcast about", name)
        end)
        urt.emitServer("chat:say", "hello", { n = 1.5, list = { "a", "b" }, flag = true })
    )")));

    TestClient anna(server, "Anna");
    Session session{server, {&anna}};
    REQUIRE(session.pump_until([&] { return log.has("broadcast about Anna"); }));

    CHECK(log.has("started client resource 'chat'"));
    CHECK(log.has("[chat] client script runs as player 1"));
    CHECK(log.has("[chat] server got Anna hello 1.5 b true nil"));
    CHECK(log.has("[chat] client got echo HELLO 42"));
    CHECK(log.count("broadcast about Anna") == 1);
    REQUIRE(anna.scripts.resource("chat").has_value());
    CHECK(anna.scripts.resource("chat")->state == ResourceState::Running);

    // Someone who joins later gets the scripts too, and a broadcast reaches
    // everyone who is connected.
    TestClient boris(server, "Boris");
    session.clients.push_back(&boris);
    REQUIRE(session.pump_until([&] { return log.count("broadcast about Boris") == 2; }));
    CHECK(log.has("[chat] server got Boris hello"));
    CHECK(log.count("client got echo HELLO 42") == 2); // each client got only its own echo
}

TEST_CASE("client scripts can look at the world and colour entities, but not change the game") {
    LogCapture log;
    TempResources files;
    TestServer server(manual_config());

    REQUIRE(server.load(files.resource("paint", R"(
        local statue = urt.spawn({ x = 3, z = 4 })
        urt.on("playerJoined", function(player)
            urt.emitClient(player.id, "paint:target", statue)
        end)
    )", R"(
        local missing = {}
        for _, name in { "spawn", "despawn", "setPosition", "setYaw", "setVelocity", "players", "emitClient",
                         "emitAllClients" } do
            if urt[name] ~= nil then table.insert(missing, name) end
        end
        for _, name in { "io", "require", "loadstring", "dofile", "package" } do
            if _G[name] ~= nil then table.insert(missing, name) end
        end
        urt.log("things a client script should not have:", #missing)
        urt.log("unknown client event:", pcall(urt.on, "playerJoined", function() end))

        local target
        urt.onNet("paint:target", function(id) target = id end)
        urt.on("tick", function()
            if target and urt.exists(target) then
                local x, y, z = urt.getPosition(target)
                urt.log(string.format("target at %.0f %.0f %.0f, i see %d entities, i am %d", x, y, z,
                                      #urt.entities(), urt.localPlayer()))
                urt.setColor(target, 0.25, 0.5, 1)
                urt.log("bad colour:", pcall(urt.setColor, target, 2, 0, 0))
                urt.log("unseen entity:", pcall(urt.getPosition, 9999))
                target = nil
            end
        end)
        urt.on("stop", function() urt.log("client part stopping") end)
    )")));

    TestClient anna(server, "Anna");
    Session session{server, {&anna}};
    REQUIRE(session.pump_until([&] { return log.has("[paint] target at 3 0 4, i see 2 entities, i am 2"); }));
    CHECK(log.has("[paint] things a client script should not have: 0"));
    CHECK(log.has("[paint] unknown client event: false"));
    CHECK(log.has("[paint] bad colour: false"));
    CHECK(log.has("[paint] unseen entity: false"));

    // The colour is a component on the client's copy of the entity.
    const Entity statue = anna.client.find(1);
    REQUIRE((statue != entt::null));
    auto& registry = anna.app.world().registry;
    REQUIRE(registry.all_of<net::Tint>(statue));
    CHECK(registry.get<net::Tint>(statue).color.b == 1.0f);
    CHECK(registry.get<net::Tint>(statue).color.r == 0.25f);

    // Stopping the resource on the server stops its client part everywhere
    // and takes the colour away again.
    CHECK(server.scripts.stop(server.app.world(), "paint"));
    REQUIRE(session.pump_until([&] { return !anna.scripts.resource("paint").has_value(); }));
    CHECK(log.has("[paint] client part stopping"));
    CHECK(anna.scripts.resources().empty());
    CHECK((anna.client.find(1) == entt::null)); // the statue went with the resource
}

TEST_CASE("a reload on the server reaches the clients") {
    LogCapture log;
    TempResources files;
    files.resource("live", "urt.log('server v1')", R"(
        urt.log("client v1")
        urt.on("stop", function() urt.log("client v1 stopping") end)
    )");

    script::ScriptConfig config;
    config.resources_directory = files.root;
    config.hot_reload = true;
    config.hot_reload_interval_seconds = 0.0;
    TestServer server(config);

    TestClient anna(server, "Anna");
    Session session{server, {&anna}};
    REQUIRE(session.pump_until([&] { return log.has("[live] client v1"); }));

    // Edit only the client script.
    const fs::path client_script = files.write("live", "client.lua", "urt.log('client v2')");
    touch(client_script, 10);
    REQUIRE(session.pump_until([&] { return log.has("[live] client v2"); }));
    CHECK(log.has("[live] client v1 stopping"));
    CHECK(log.count("[live] server v1") == 2); // the server part restarted as well
    CHECK(anna.scripts.resources().size() == 1);

    // A broken server script stops the resource for the clients too.
    const fs::path server_script = files.write("live", "server.lua", "this is not lua");
    touch(server_script, 20);
    REQUIRE(session.pump_until([&] { return anna.scripts.resources().empty(); }));

    // Fixed again: the clients get it back.
    files.write("live", "server.lua", "urt.log('server v3')");
    files.write("live", "client.lua", "urt.log('client v3')");
    touch(server_script, 30);
    REQUIRE(session.pump_until([&] { return log.has("[live] client v3"); }));
}

TEST_CASE("a client survives hostile scripts from a server") {
    LogCapture log;
    TempResources files;
    TestServer server(manual_config());

    REQUIRE(server.load(files.resource("aaa-spinner", "", "while true do end")));
    REQUIRE(server.load(files.resource("bbb-hog", "", R"(
        local hoard = {}
        while true do
            pcall(function() table.insert(hoard, table.create(10000, 0)) end)
        end
    )")));
    REQUIRE(server.load(files.resource("ccc-prober", "", R"(
        local found = {}
        for _, name in { "io", "require", "loadstring", "dofile", "loadfile", "package", "load" } do
            if _G[name] ~= nil then table.insert(found, name) end
        end
        for _, name in { "execute", "remove", "rename", "getenv", "exit" } do
            if os[name] ~= nil then table.insert(found, "os." .. name) end
        end
        urt.log("ways out of the sandbox:", #found)
        urt.log("line one\nforged: [admin] line two")
    )")));
    REQUIRE(server.load(files.resource("ddd-honest", "", "urt.every(0.1, function() urt.log('still here') end)")));

    script::ClientScriptConfig limits;
    limits.max_call_seconds = 0.05;
    limits.memory_limit_bytes = 8u * 1024u * 1024u;
    TestClient anna(server, "Anna", limits);
    Session session{server, {&anna}};

    REQUIRE(session.pump_until([&] { return log.count("[ddd-honest] still here") >= 3; }));

    REQUIRE(anna.scripts.resource("aaa-spinner").has_value());
    CHECK(anna.scripts.resource("aaa-spinner")->state == ResourceState::Failed);
    CHECK(anna.scripts.resource("aaa-spinner")->error.find("ran for too long") != std::string::npos);
    CHECK(anna.scripts.resource("bbb-hog")->state == ResourceState::Failed);
    CHECK(anna.scripts.resource("bbb-hog")->error.find("too much memory") != std::string::npos);
    CHECK(log.has("[ccc-prober] ways out of the sandbox: 0"));
    // A script cannot start a new log line to impersonate something else.
    CHECK(log.has("[ccc-prober] line one forged: [admin] line two"));
    CHECK(anna.scripts.resource("ddd-honest")->state == ResourceState::Running);
    CHECK(anna.client.state() == net::ClientState::Connected);

    // On the server all four are fine: their server parts are empty.
    CHECK(server.scripts.resource("aaa-spinner")->state == ResourceState::Running);
}

TEST_CASE("only plain data can be sent in an event") {
    LogCapture log;
    TempResources files;
    TestServer server(manual_config());

    REQUIRE(server.load(files.resource("codec", R"(
        urt.onNet("codec:ok", function(player, value)
            urt.log("deep value:", value.a.b.c.d[1], "empty:", next(value.empty) == nil, "text:", value.text)
        end)
        urt.onNet("codec:many", function(player, ...)
            urt.log("argument count:", select("#", ...), "last:", select(-1, ...))
        end)
        urt.onNet("codec:none", function(player, ...)
            urt.log("no arguments:", select("#", ...))
        end)
    )", R"(
        local function try(what, ...)
            urt.log(what, pcall(urt.emitServer, "codec:rejected", ...) and "sent" or "refused")
        end
        local cyclic = {}
        cyclic.self = cyclic
        local deep = {}
        local level = deep
        for i = 1, 12 do level.next = {} level = level.next end

        try("function", function() end)
        try("function in table", { callback = print })
        try("coroutine", coroutine.create(function() end))
        try("cyclic table", cyclic)
        try("deep table", deep)
        try("nan", 0/0)
        try("infinity", math.huge)
        try("boolean key", { [true] = 1 })
        try("long string", string.rep("x", 9000))
        try("big event", table.create(3000, "0123456789"))
        try("17 arguments", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17)
        urt.log("bad name", pcall(urt.emitServer, "has space") and "sent" or "refused")
        urt.log("empty name", pcall(urt.emitServer, "") and "sent" or "refused")

        urt.emitServer("codec:ok", { a = { b = { c = { d = { "bottom" } } } }, empty = {}, text = "ž ľ š" })
        urt.emitServer("codec:many", 1, "two", true, nil, { 5 }, 6)
        urt.emitServer("codec:none")
    )")));

    TestClient anna(server, "Anna");
    Session session{server, {&anna}};
    REQUIRE(session.pump_until([&] { return log.has("[codec] no arguments: 0"); }));

    for (const char* refused : {"function", "function in table", "coroutine", "cyclic table", "deep table", "nan",
                                "infinity", "boolean key", "long string", "big event", "17 arguments", "bad name",
                                "empty name"}) {
        CHECK_MESSAGE(log.has(std::string("[codec] ") + refused + " refused"), refused);
    }
    CHECK(log.has("[codec] deep value: bottom empty: true text: ž ľ š"));
    CHECK(log.has("[codec] argument count: 6 last: 6"));
    CHECK_FALSE(log.has("codec:rejected"));
}

TEST_CASE("the server withstands a client that abuses events") {
    LogCapture log;
    TempResources files;
    net::ServerConfig net_config;
    net_config.event_burst = 20.0;
    net_config.event_rate_per_second = 10.0;
    net_config.max_dropped_events = 100;
    TestServer server(manual_config(), net_config);

    REQUIRE(server.load(files.resource("target", R"(
        local calls = 0
        urt.onNet("target:ping", function(player, value)
            calls += 1
            urt.log("ping", calls, type(value))
        end)
    )", "")));

    Session session{server, {}};
    RawPlayer raw(server.server.port());
    const auto pump_raw_until = [&](const std::function<bool()>& condition) {
        return session.pump_until([&] {
            raw.poll();
            return condition();
        });
    };

    SUBCASE("arguments that are not a valid encoding never reach a handler") {
        REQUIRE(pump_raw_until([&] { return raw.player_id != 0; }));
        raw.send_event("target:ping", {1, 99, 99, 99});                // one argument of an unknown kind
        raw.send_event("target:ping", {1, 5, 0xFF, 0xFF});             // a table claiming 65535 entries
        raw.send_event("target:ping", {200});                          // 200 arguments, none present
        raw.send_event("target:ping", {1, 4, 0xFF, 0x1F, 'a'});        // a string longer than the packet
        raw.send_event("target:ping", {1, 3, 0, 0, 0, 0, 0, 0, 0xF8, 0x7F}); // the number NaN
        raw.send_event("target:ping", {});                             // not even a count
        raw.send_event("nobody:listens", {0});                         // no handler anywhere
        raw.send_event("target:ping", {1, 2});                         // finally a valid one: true
        REQUIRE(pump_raw_until([&] { return log.has("[target] ping 1 boolean"); }));
        CHECK(log.count("[target] ping") == 1);
        CHECK(server.server.player_count() == 1); // sloppy arguments alone are not a reason to kick
        CHECK(server.scripts.resource("target")->state == ResourceState::Running);
    }

    SUBCASE("a burst over the allowance is trimmed, the player stays") {
        REQUIRE(pump_raw_until([&] { return raw.player_id != 0; }));
        for (int i = 0; i < 60; ++i) {
            raw.send_event("target:ping", {1, 2});
        }
        REQUIRE(pump_raw_until([&] { return log.count("[target] ping") >= 20; }));
        session.pump(5);
        raw.poll();
        CHECK(log.count("[target] ping") <= 22); // the burst of 20 plus what refilled meanwhile
        CHECK(log.has("sends script events too fast"));
        CHECK_FALSE(raw.dropped);
        CHECK(server.server.player_count() == 1);

        // After a pause the allowance is back.
        session.pump(60); // 1.2 s at 10 events per second
        raw.send_event("target:ping", {1, 2});
        const int before = log.count("[target] ping");
        REQUIRE(pump_raw_until([&] { return log.count("[target] ping") == before + 1; }));
    }

    SUBCASE("a flood gets the sender disconnected") {
        REQUIRE(pump_raw_until([&] { return raw.player_id != 0; }));
        for (int i = 0; i < 500; ++i) {
            raw.send_event("target:ping", {1, 2});
        }
        REQUIRE(pump_raw_until([&] { return raw.dropped; }));
        // At most the allowance got through; events from a player who is
        // kicked in the same tick are not delivered at all.
        CHECK(log.count("[target] ping") <= 25);
        CHECK(log.has("event flood"));
        CHECK(server.server.player_count() == 0);
        CHECK(server.scripts.resource("target")->state == ResourceState::Running);
    }

    SUBCASE("an event that is not even a valid message disconnects the sender") {
        REQUIRE(pump_raw_until([&] { return raw.player_id != 0; }));
        const std::vector<std::uint8_t> broken{static_cast<std::uint8_t>(net::MessageType::NetEvent), 3, 'a', 'b'};
        raw.send(broken);
        REQUIRE(pump_raw_until([&] { return raw.dropped; }));
        CHECK(log.has("malformed event"));
    }

    SUBCASE("events are refused before the handshake") {
        raw.say_hello = false;
        REQUIRE(pump_raw_until([&] {
            return std::any_of(raw.events.begin(), raw.events.end(),
                               [](const net::Event& e) { return e.type == net::Event::Type::Connected; });
        }));
        raw.send_event("target:ping", {1, 2});
        REQUIRE(pump_raw_until([&] { return raw.dropped; }));
        CHECK(log.has("unexpected event"));
        CHECK_FALSE(log.has("[target] ping"));
    }
}

TEST_CASE("a server script that crashes on an event stops for its clients as well") {
    LogCapture log;
    TempResources files;
    script::ScriptConfig config = manual_config();
    config.max_call_seconds = 0.05;
    TestServer server(config);

    REQUIRE(server.load(files.resource("fragile", R"(
        urt.onNet("fragile:spin", function() while true do end end)
    )", R"(
        urt.log("fragile client part runs")
        urt.after(0.2, function() urt.emitServer("fragile:spin") end)
    )")));

    TestClient anna(server, "Anna");
    Session session{server, {&anna}};
    REQUIRE(session.pump_until([&] { return log.has("fragile client part runs"); }));
    REQUIRE(session.pump_until([&] { return server.scripts.resource("fragile")->state == ResourceState::Failed; }));
    REQUIRE(session.pump_until([&] { return anna.scripts.resources().empty(); }));
    CHECK(anna.client.state() == net::ClientState::Connected);
}

TEST_CASE("client scripts end with the connection and return on reconnect") {
    LogCapture log;
    TempResources files;
    TestServer server(manual_config());
    REQUIRE(server.load(files.resource("session", "", R"(
        urt.log("client part started")
        urt.on("stop", function() urt.log("client part stopped") end)
    )")));

    TestClient anna(server, "Anna");
    Session session{server, {&anna}};
    REQUIRE(session.pump_until([&] { return log.count("[session] client part started") == 1; }));

    anna.client.disconnect();
    REQUIRE(session.pump_until([&] { return anna.scripts.resources().empty(); }));
    CHECK(log.count("[session] client part stopped") == 1);
    REQUIRE(session.pump_until([&] { return server.server.player_count() == 0; }));

    REQUIRE(anna.client.connect());
    REQUIRE(session.pump_until([&] { return log.count("[session] client part started") == 2; }));
    CHECK(anna.scripts.resources().size() == 1);
}
