// Scripting tests: real resources written to a temporary folder and run by
// the real ScriptSystem.

#include <doctest/doctest.h>

#include "urt/app/application.hpp"
#include "urt/core/log.hpp"
#include "urt/ecs/components.hpp"
#include "urt/ecs/movement_system.hpp"
#include "urt/net/client.hpp"
#include "urt/net/server.hpp"
#include "urt/script/script_system.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace urt;
using urt::script::ResourceState;

namespace {

// Collects log lines while it lives, so tests can check what scripts printed
// and which errors were reported. Restores normal logging afterwards.
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

// A resources folder that is deleted again when the test ends.
struct TempResources {
    fs::path root;

    TempResources() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() /
               ("urt-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(counter.fetch_add(1)));
        fs::create_directories(root);
    }
    ~TempResources() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    // Writes one file of a resource, creating the folder if needed.
    fs::path write(const std::string& resource, const std::string& file, const std::string& content) const {
        const fs::path path = root / resource / file;
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << content;
        return path;
    }

    // A resource with the usual manifest and one server script.
    fs::path resource(const std::string& name, const std::string& server_script) const {
        write(name, "resource.lua", "return { server = { 'server.lua' } }\n");
        write(name, "server.lua", server_script);
        return root / name;
    }
};

// Marks a file as changed. File times can be too coarse to notice two writes
// within the same test, so the time is pushed forward explicitly.
void touch(const fs::path& path, int seconds_ahead) {
    fs::last_write_time(path, fs::file_time_type::clock::now() + std::chrono::seconds(seconds_ahead));
}

struct ScriptServer {
    Application app;
    net::Server& server;
    script::ScriptSystem& scripts;

    static AppConfig app_config() {
        AppConfig config;
        config.tick_rate = 50.0; // dt = 0.02 s
        config.stats_interval_seconds = 0.0;
        config.worker_threads = 1;
        return config;
    }

    explicit ScriptServer(script::ScriptConfig config = {})
        : app(app_config()),
          server(app.add_system<net::Server>(net::ServerConfig{.port = 0})),
          scripts(app.add_system<script::ScriptSystem>(server, [&] {
              config.slow_call_seconds = 10.0; // no "slow script" noise from a loaded test machine
              return config;
          }())) {
        app.add_system<MovementSystem>();
        app.add_system<net::ServerSnapshotSystem>(server);
    }

    bool load(const fs::path& directory) { return scripts.load(app.world(), directory); }

    script::ResourceInfo info(const std::string& name) const {
        const auto found = scripts.resource(name);
        REQUIRE(found.has_value());
        return *found;
    }
};

script::ScriptConfig manual_config() {
    script::ScriptConfig config;
    config.hot_reload = false;
    return config;
}

} // namespace

TEST_CASE("a resource loads, runs and logs") {
    LogCapture log;
    TempResources files;
    ScriptServer s(manual_config());

    REQUIRE(s.load(files.resource("hello", R"(
        local answer = 6 * 7
        urt.log("hello from", urt.resourceName, answer, true, nil)
        print("print works too")
    )")));

    CHECK(s.info("hello").state == ResourceState::Running);
    CHECK(s.info("hello").error.empty());
    CHECK(log.has("script: [hello] hello from hello 42 true nil"));
    CHECK(log.has("script: [hello] print works too"));
    CHECK(log.has("started resource 'hello'"));
}

TEST_CASE("scripts spawn, move and remove entities") {
    LogCapture log;
    TempResources files;
    ScriptServer s(manual_config());

    REQUIRE(s.load(files.resource("mover", R"(
        local walker = urt.spawn({ x = 5, z = -2, yaw = 1 })
        local statue = urt.spawn()
        urt.setVelocity(walker, 1, 0, 0)

        urt.after(1, function()
            local x, y, z = urt.getPosition(walker)
            urt.log(string.format("walker at %.2f %.2f %.2f yaw %.1f", x, y, z, urt.getYaw(walker)))
            urt.setPosition(statue, 100, 0, 100)
            urt.log("statue exists:", urt.exists(statue), "despawned:", urt.despawn(statue))
            urt.log("statue exists:", urt.exists(statue))
        end)
    )")));

    CHECK(s.app.world().entity_count() == 2);
    CHECK(s.info("mover").entities == 2);

    s.app.step(52); // a little over one second at 50 ticks/s
    // 1 m/s for one second from x = 5. The timer runs before that tick's
    // movement, hence 6.00 give or take a tick.
    CHECK(log.has("[mover] walker at 6.0"));
    CHECK(log.has("yaw 1.0"));
    CHECK(log.has("[mover] statue exists: true despawned: true"));
    CHECK(log.has("[mover] statue exists: false"));
    CHECK(s.app.world().entity_count() == 1);
    CHECK(s.info("mover").entities == 1);
}

TEST_CASE("timers fire once or repeatedly and can be cleared") {
    LogCapture log;
    TempResources files;
    ScriptServer s(manual_config());

    REQUIRE(s.load(files.resource("timers", R"(
        urt.after(0.5, function() urt.log("once") end)

        local beats = 0
        urt.every(0.1, function()
            beats += 1
            urt.log("beat", beats)
        end)

        local runs = 0
        local limited
        limited = urt.every(0.1, function()
            runs += 1
            urt.log("limited", runs)
            if runs == 3 then
                urt.log("cleared:", urt.clearTimer(limited), "again:", urt.clearTimer(limited))
            end
        end)

        local never = urt.after(0.2, function() urt.log("never") end)
        urt.clearTimer(never)

        urt.log("time at start:", urt.time())
    )")));

    s.app.step(55); // 1.1 simulated seconds
    CHECK(log.count("[timers] once") == 1);
    CHECK(log.count("[timers] beat") >= 9);
    CHECK(log.count("[timers] beat") <= 11);
    CHECK(log.count("[timers] limited") == 3);
    CHECK(log.has("[timers] cleared: true again: false"));
    CHECK_FALSE(log.has("[timers] never"));
    CHECK(log.has("[timers] time at start: 0"));
}

TEST_CASE("resources are isolated from each other and cannot change the built-ins") {
    LogCapture log;
    TempResources files;
    ScriptServer s(manual_config());

    const std::string counting = R"(
        counter = (counter or 0) + 1   -- a global variable
        urt.log("counter is", counter)
    )";
    REQUIRE(s.load(files.resource("first", counting)));
    REQUIRE(s.load(files.resource("second", counting)));
    CHECK(log.count("counter is 1") == 2); // each saw its own, fresh global

    // Several files of one resource do share globals.
    files.write("shared", "resource.lua", "return { server = { 'a.lua', 'b.lua' } }");
    files.write("shared", "a.lua", "function greet(name) return 'hi ' .. name end");
    files.write("shared", "b.lua", "urt.log(greet('b'))");
    REQUIRE(s.load(files.root / "shared"));
    CHECK(log.has("[shared] hi b"));

    // The standard library and the urt table are read-only.
    CHECK_FALSE(s.load(files.resource("vandal", "string.rep = nil")));
    CHECK(s.info("vandal").error.find("readonly") != std::string::npos);
    CHECK_FALSE(s.load(files.resource("vandal2", "urt.spawn = function() end")));
    CHECK(s.info("vandal2").state == ResourceState::Failed);
}

TEST_CASE("scripts have no access to files, processes or code loading") {
    LogCapture log;
    TempResources files;
    ScriptServer s(manual_config());

    REQUIRE(s.load(files.resource("probe", R"(
        local missing = {}
        for _, name in { "io", "require", "dofile", "loadfile", "load", "loadstring", "package" } do
            if _G[name] ~= nil then table.insert(missing, name) end
        end
        for _, name in { "execute", "remove", "rename", "getenv", "exit", "tmpname" } do
            if os[name] ~= nil then table.insert(missing, "os." .. name) end
        end
        urt.log("dangerous functions found:", #missing, table.concat(missing, ","))
        urt.log("os.clock is a", type(os.clock))
    )")));
    CHECK(log.has("[probe] dangerous functions found: 0"));
    CHECK(log.has("[probe] os.clock is a function"));
}

TEST_CASE("script errors are reported with file and line") {
    LogCapture log;
    TempResources files;
    ScriptServer s(manual_config());

    SUBCASE("a syntax error stops the resource from loading") {
        CHECK_FALSE(s.load(files.resource("broken", "local x = 1\nlocal y = = 2\n")));
        const auto info = s.info("broken");
        CHECK(info.state == ResourceState::Failed);
        CHECK(info.error.find("broken/server.lua:2") != std::string::npos);
        CHECK(log.has("[broken] stopped"));
    }

    SUBCASE("an error while loading removes what was already spawned") {
        CHECK_FALSE(s.load(files.resource("half", "urt.spawn()\nurt.spawn()\nerror('gave up')\n")));
        CHECK(s.info("half").error.find("half/server.lua:3: gave up") != std::string::npos);
        CHECK(s.app.world().entity_count() == 0);
    }

    SUBCASE("an error in a handler is logged and the resource keeps running") {
        REQUIRE(s.load(files.resource("flaky", R"(
            local ticks = 0
            urt.on("tick", function()
                ticks += 1
                if ticks == 2 then
                    local nothing = nil
                    nothing.field = 1 -- a typical mistake
                end
                urt.log("tick", ticks)
            end)
        )")));
        s.app.step(4);
        CHECK(log.count("[flaky] error in the 'tick' handler") == 1);
        CHECK(log.has("flaky/server.lua:7"));
        CHECK(log.has("[flaky] tick 1"));
        CHECK_FALSE(log.has("[flaky] tick 2")); // the failing call stopped there
        CHECK(log.has("[flaky] tick 4"));       // but later ticks still run
        CHECK(s.info("flaky").state == ResourceState::Running);
    }
}

TEST_CASE("wrong arguments raise script errors instead of corrupting the world") {
    LogCapture log;
    TempResources files;
    ScriptServer s(manual_config());

    REQUIRE(s.load(files.resource("careless", R"(
        local id = urt.spawn({ x = 1 })
        local function try(what, fn, ...)
            local ok, message = pcall(fn, ...)
            urt.log(what, ok and "accepted" or "refused")
        end
        try("nan position", urt.setPosition, id, 0/0, 0, 0)
        try("infinite position", urt.setPosition, id, math.huge, 0, 0)
        try("far position", urt.setPosition, id, 1e30, 0, 0)
        try("text position", urt.setPosition, id, "here", 0, 0)
        try("huge speed", urt.setVelocity, id, 1e9, 0, 0)
        try("nan yaw", urt.setYaw, id, 0/0)
        try("nan spawn", urt.spawn, { x = 0/0 })
        try("text spawn", urt.spawn, { x = "left" })
        try("unknown entity", urt.getPosition, 424242)
        try("foreign despawn", urt.despawn, 424242)
        try("fractional id", urt.exists, 1.5)
        try("unknown event", urt.on, "playerJoinned", function() end)
        try("handler not a function", urt.on, "tick", 5)
        try("negative timer", urt.after, -1, function() end)
        try("good position", urt.setPosition, id, 3, 0, 4)
        local x, y, z = urt.getPosition(id)
        urt.log("position is", x, y, z)
    )")));

    for (const char* refused :
         {"nan position", "infinite position", "far position", "text position", "huge speed", "nan yaw", "nan spawn",
          "text spawn", "unknown entity", "foreign despawn", "fractional id", "unknown event",
          "handler not a function", "negative timer"}) {
        CHECK_MESSAGE(log.has(std::string("[careless] ") + refused + " refused"), refused);
    }
    CHECK(log.has("[careless] good position accepted"));
    CHECK(log.has("[careless] position is 3 0 4"));
    CHECK(s.app.world().entity_count() == 1); // the refused spawns created nothing
}

TEST_CASE("a script that never stops is stopped, and the server carries on") {
    LogCapture log;
    TempResources files;
    script::ScriptConfig config = manual_config();
    config.max_call_seconds = 0.05;
    ScriptServer s(config);

    // Something well-behaved that must keep working throughout.
    REQUIRE(s.load(files.resource("good", "urt.on('tick', function() beats = (beats or 0) + 1 end)\n"
                                          "urt.every(0.1, function() urt.log('alive', beats) end)")));

    const auto started = std::chrono::steady_clock::now();

    SUBCASE("while loading") {
        CHECK_FALSE(s.load(files.resource("spinner", "urt.spawn()\nwhile true do end")));
        CHECK(s.info("spinner").error.find("ran for too long") != std::string::npos);
        CHECK(s.app.world().entity_count() == 0); // what it had spawned is gone
    }

    SUBCASE("in a handler, hiding inside pcall") {
        REQUIRE(s.load(files.resource("spinner", R"(
            urt.on("tick", function()
                while true do
                    pcall(function() while true do end end)
                end
            end)
        )")));
        s.app.step();
        CHECK(s.info("spinner").state == ResourceState::Failed);
        CHECK(s.info("spinner").error.find("ran for too long") != std::string::npos);
    }

    SUBCASE("inside a library callback, where it cannot simply be suspended") {
        REQUIRE(s.load(files.resource("spinner", R"(
            urt.on("tick", function()
                while true do
                    pcall(table.sort, { 3, 1, 2 }, function(a, b)
                        while true do
                            pcall(function() while true do end end)
                        end
                    end)
                end
            end)
        )")));
        s.app.step();
        CHECK(s.info("spinner").state == ResourceState::Failed);
    }

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK(seconds < 5.0); // stopped by the limit, not by luck

    s.app.step(20);
    CHECK(log.has("[good] alive"));
    CHECK(s.info("good").state == ResourceState::Running);
}

TEST_CASE("a script that eats memory is stopped at its limit") {
    LogCapture log;
    TempResources files;
    script::ScriptConfig config = manual_config();
    config.memory_limit_bytes = 8u * 1024u * 1024u;
    config.max_call_seconds = 5.0;
    ScriptServer s(config);

    REQUIRE(s.load(files.resource("good", "urt.on('tick', function() end)")));

    SUBCASE("at load") {
        CHECK_FALSE(s.load(files.resource("hog", R"(
            -- Luau stores equal strings only once, so hoard tables instead.
            local hoard = {}
            while true do
                table.insert(hoard, table.create(10000, 0))
            end
        )")));
        CHECK_MESSAGE(s.info("hog").error.find("too much memory") != std::string::npos, s.info("hog").error);
    }

    SUBCASE("in a handler, while catching its own errors") {
        REQUIRE(s.load(files.resource("hog", R"(
            local hoard = {}
            urt.on("tick", function()
                while true do
                    pcall(function() table.insert(hoard, table.create(10000, 0)) end)
                end
            end)
        )")));
        s.app.step();
        CHECK(s.info("hog").state == ResourceState::Failed);
    }

    CHECK(s.info("hog").memory_bytes == 0); // its memory was given back
    s.app.step(5);
    CHECK(s.info("good").state == ResourceState::Running);
}

TEST_CASE("a resource cannot spawn more entities than its limit") {
    LogCapture log;
    TempResources files;
    script::ScriptConfig config = manual_config();
    config.max_entities_per_resource = 10;
    ScriptServer s(config);

    REQUIRE(s.load(files.resource("crowd", R"(
        local spawned = {}
        urt.on("tick", function()
            for i = 1, 25 do
                table.insert(spawned, urt.spawn())
            end
        end)
        -- After a despawn there is room for one more.
        urt.after(0.1, function()
            urt.despawn(spawned[1])
            urt.log("room again:", pcall(urt.spawn))
        end)
    )")));
    s.app.step();
    CHECK(s.app.world().entity_count() == 10);
    CHECK(log.has("the most allowed"));
    CHECK(s.info("crowd").state == ResourceState::Running);

    s.app.step(6);
    CHECK(log.has("[crowd] room again: true"));
    CHECK(s.app.world().entity_count() == 10);
}

TEST_CASE("stopping a resource runs its stop handler and removes its entities") {
    LogCapture log;
    TempResources files;
    ScriptServer s(manual_config());

    REQUIRE(s.load(files.resource("temporary", R"(
        urt.spawn()
        urt.spawn()
        urt.on("stop", function() urt.log("cleaning up") end)
    )")));
    REQUIRE(s.load(files.resource("permanent", "urt.spawn()")));
    CHECK(s.app.world().entity_count() == 3);

    CHECK(s.scripts.stop(s.app.world(), "temporary"));
    CHECK(log.has("[temporary] cleaning up"));
    CHECK(s.app.world().entity_count() == 1); // the other resource's entity stays
    CHECK_FALSE(s.scripts.resource("temporary").has_value());
    CHECK_FALSE(s.scripts.stop(s.app.world(), "temporary"));

    // Loading a resource that is already loaded replaces it.
    files.write("permanent", "server.lua", "urt.spawn() urt.spawn() urt.spawn()");
    REQUIRE(s.load(files.root / "permanent"));
    CHECK(s.app.world().entity_count() == 3);
    CHECK(s.scripts.resources().size() == 1);
}

TEST_CASE("manifests are validated") {
    LogCapture log;
    TempResources files;
    ScriptServer s(manual_config());

    SUBCASE("no manifest") {
        fs::create_directories(files.root / "empty");
        CHECK_FALSE(s.load(files.root / "empty"));
        CHECK(s.info("empty").error.find("cannot open resource.lua") != std::string::npos);
    }
    SUBCASE("not a table") {
        files.write("odd", "resource.lua", "return 42");
        CHECK_FALSE(s.load(files.root / "odd"));
        CHECK(s.info("odd").error.find("must return a table") != std::string::npos);
    }
    SUBCASE("a script outside the resource folder") {
        files.write("sneaky", "resource.lua", "return { server = { '../other/server.lua' } }");
        CHECK_FALSE(s.load(files.root / "sneaky"));
        CHECK(s.info("sneaky").error.find("not a valid script path") != std::string::npos);
    }
    SUBCASE("a script that is not there") {
        files.write("forgetful", "resource.lua", "return { server = { 'missing.lua' } }");
        CHECK_FALSE(s.load(files.root / "forgetful"));
        CHECK(s.info("forgetful").error.find("cannot open missing.lua") != std::string::npos);
    }
    SUBCASE("a name that differs from the folder") {
        files.write("folder", "resource.lua", "return { name = 'other' }");
        CHECK_FALSE(s.load(files.root / "folder"));
        CHECK(s.info("folder").error.find("must be the folder's name") != std::string::npos);
    }
    SUBCASE("the manifest has no game API and cannot run forever") {
        files.write("greedy", "resource.lua", "urt.spawn() return {}");
        CHECK_FALSE(s.load(files.root / "greedy"));
        files.write("endless", "resource.lua", "while true do end");
        CHECK_FALSE(s.load(files.root / "endless"));
        CHECK(s.info("endless").error.find("ran for too long") != std::string::npos);
        CHECK(s.app.world().entity_count() == 0);
    }
    SUBCASE("a folder name with characters that are not allowed") {
        files.write("Bad Name", "resource.lua", "return {}");
        CHECK_FALSE(s.load(files.root / "Bad Name"));
        CHECK(s.scripts.resources().empty());
    }
    SUBCASE("a manifest with no scripts is a valid, empty resource") {
        files.write("quiet", "resource.lua", "return { name = 'quiet', version = '1.0.0' }");
        CHECK(s.load(files.root / "quiet"));
        CHECK(s.info("quiet").state == ResourceState::Running);
    }
}

TEST_CASE("resources load on start and reload when their files change") {
    LogCapture log;
    TempResources files;
    files.resource("alpha", "urt.spawn() urt.spawn()\nurt.on('stop', function() urt.log('alpha stopping') end)");
    files.resource("beta", "urt.spawn()");

    script::ScriptConfig config;
    config.resources_directory = files.root;
    config.hot_reload = true;
    config.hot_reload_interval_seconds = 0.0; // check on every tick
    ScriptServer s(config);

    s.app.step(); // the first tick loads everything in the folder
    CHECK(s.scripts.resources().size() == 2);
    CHECK(s.info("alpha").state == ResourceState::Running);
    CHECK(s.app.world().entity_count() == 3);

    // Edit a script: the old instance is stopped, the new one started.
    const fs::path script = files.write("alpha", "server.lua", "for i = 1, 5 do urt.spawn() end");
    touch(script, 10);
    s.app.step();
    CHECK(log.has("[alpha] alpha stopping"));
    CHECK(log.has("files of resource 'alpha' changed"));
    CHECK(s.info("alpha").entities == 5);
    CHECK(s.app.world().entity_count() == 6);

    // Nothing changed: no reload.
    const int reloads = log.count("changed, reloading");
    s.app.step(5);
    CHECK(log.count("changed, reloading") == reloads);

    // A broken edit stops the resource and removes its entities...
    files.write("alpha", "server.lua", "this is not lua");
    touch(script, 20);
    s.app.step();
    CHECK(s.info("alpha").state == ResourceState::Failed);
    CHECK(s.app.world().entity_count() == 1);
    s.app.step(3);
    CHECK(log.count("[alpha] stopped") == 1); // reported once, not on every check

    // ...and fixing it brings the resource back.
    files.write("alpha", "server.lua", "urt.spawn()");
    touch(script, 30);
    s.app.step();
    CHECK(s.info("alpha").state == ResourceState::Running);
    CHECK(s.app.world().entity_count() == 2);

    // A new folder is picked up, a deleted one is stopped.
    files.resource("gamma", "urt.spawn() urt.spawn() urt.spawn()");
    s.app.step();
    CHECK(s.info("gamma").state == ResourceState::Running);
    CHECK(s.app.world().entity_count() == 5);

    fs::remove_all(files.root / "beta");
    s.app.step();
    CHECK_FALSE(s.scripts.resource("beta").has_value());
    CHECK(s.app.world().entity_count() == 4);
}

TEST_CASE("scripts see players join and leave, and clients see what scripts spawn") {
    LogCapture log;
    TempResources files;
    ScriptServer s(manual_config());
    REQUIRE(s.server.start());

    REQUIRE(s.load(files.resource("welcome", R"(
        urt.on("playerJoined", function(player)
            urt.log(string.format("%s joined as %d at %.1f %.1f, %d online", player.name, player.id, player.x,
                                  player.z, #urt.players()))
            urt.spawn({ x = player.x + 2, z = player.z })
        end)
        urt.on("playerLeft", function(player)
            urt.log(player.name, "left,", #urt.players(), "online")
        end)
        urt.on("tick", function()
            for _, player in urt.players() do
                local x = urt.getPosition(player.id)
                if x > 50 then
                    urt.setPosition(player.id, 0, 0, 0) -- walked too far: back to the start
                    urt.log("sent", player.name, "back")
                end
            end
        end)
    )")));

    Application client_app(ScriptServer::app_config());
    auto& client = client_app.add_system<net::Client>(
        net::ClientConfig{.host = "127.0.0.1", .port = s.server.port(), .name = "Anna"});
    REQUIRE(client.connect());

    const auto pump_until = [&](const auto& condition) {
        for (int i = 0; i < 3000 && !condition(); ++i) {
            s.app.step();
            client_app.step();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return condition();
    };

    // The player and the greeter the script spawned next to them.
    REQUIRE(pump_until([&] { return client.visible_count() == 2; }));
    CHECK(log.has("[welcome] Anna joined as 1 at"));
    CHECK(log.has("1 online"));

    // A script teleports the player; the client follows.
    const Entity on_server = s.server.player_entity(client.player_id());
    s.app.world().registry.get<Transform>(on_server).position.x = 60.0f;
    REQUIRE(pump_until([&] { return log.has("[welcome] sent Anna back"); }));
    net::Pose drawn;
    REQUIRE(pump_until([&] { return client.predicted_pose(1.0f, drawn) && drawn.position.x == 0.0f; }));

    client.disconnect();
    REQUIRE(pump_until([&] { return log.has("[welcome] Anna left, 0 online"); }));
}
