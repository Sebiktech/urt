#pragma once

// Server-side scripting: loads resources and runs their Luau scripts.
//
// A resource is a folder with a manifest (resource.lua) and scripts. Each
// resource gets its own virtual machine, so resources cannot see or disturb
// each other, and one that misbehaves is stopped without taking the server
// down. Scripts reach the game only through the `urt` table; docs/scripting.md
// lists what is in it.
//
// Add this system after net::Server and before the systems that move things:
// scripts then see this tick's joins and leaves, and what they change is
// simulated and sent in the same tick.

#include "urt/ecs/system.hpp"
#include "urt/ecs/world.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace urt::net {
class Server;
}

namespace urt::script {

struct ScriptConfig {
    // Folder whose sub-folders are resources. Empty: load nothing on start.
    std::filesystem::path resources_directory;

    // A single call into a script (an event handler, a timer, loading a
    // file) that runs longer than this is stopped together with its
    // resource. 0 turns the limit off, which helps when stepping through the
    // server in a debugger.
    double max_call_seconds = 0.25;

    // Calls slower than this get a warning in the log (at most one per
    // resource every few seconds).
    double slow_call_seconds = 0.005;

    // Memory one resource may use.
    std::size_t memory_limit_bytes = 64u * 1024u * 1024u;

    // Entities one resource may have spawned at the same time.
    std::size_t max_entities_per_resource = 4096;

    // Reload a resource when one of its files changes on disk.
    bool hot_reload = true;
    double hot_reload_interval_seconds = 0.5;
};

enum class ResourceState {
    Running,
    Failed, // could not be loaded, or was stopped for breaking a limit
};

struct ResourceInfo {
    std::string name;
    ResourceState state = ResourceState::Failed;
    std::string error;          // why it failed; empty while running
    std::size_t entities = 0;   // entities it has spawned and not removed
    std::size_t memory_bytes = 0;
};

class ScriptSystem final : public System {
public:
    ScriptSystem(net::Server& server, ScriptConfig config);
    ~ScriptSystem() override;

    std::string_view name() const override { return "script"; }

    // Loads everything in resources_directory on the first call. Then, every
    // tick: player events, timers and "tick" handlers, and the reload check.
    void tick(World& world, const TickContext& context) override;

    // Loads the resource in `directory`, replacing a loaded one of the same
    // name. Returns true if it is running afterwards.
    bool load(World& world, const std::filesystem::path& directory);

    // Stops a resource: its "stop" handlers run, then everything it spawned
    // is removed. Returns false if no such resource is loaded.
    bool stop(World& world, std::string_view name);

    std::vector<ResourceInfo> resources() const;
    std::optional<ResourceInfo> resource(std::string_view name) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace urt::script
