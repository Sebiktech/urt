#pragma once

// What the server's and the client's script systems have in common: resources
// with their virtual machines, event handlers, timers, the shared part of the
// urt.* API, and the rules for calling into scripts safely.
//
// Internal to the scripting module.

#include "vm.hpp"

#include "urt/core/time.hpp"
#include "urt/net/protocol.hpp"
#include "urt/script/script_system.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct lua_State;

namespace urt::script {

class HostBase;

struct Timer {
    int id = 0;
    int handler = 0;       // registry reference to the function
    double due = 0.0;      // host time at which it fires next
    double interval = 0.0; // 0 = fires once
};

struct Resource {
    HostBase* host = nullptr;
    std::string name;
    ResourceState state = ResourceState::Failed;
    std::string error;

    std::unique_ptr<Vm> vm;
    std::unordered_map<std::string, std::vector<int>> handlers;     // engine events -> functions
    std::unordered_map<std::string, std::vector<int>> net_handlers; // network events -> functions
    std::size_t handler_count = 0;
    std::vector<Timer> timers;
    int next_timer_id = 1;
    Clock::time_point last_slow_warning{};

    // Server side.
    std::filesystem::path directory;
    std::unordered_set<net::NetId> entities; // spawned by this resource, removed with it
    std::vector<net::ScriptFile> client_scripts;
    bool clients_have_it = false; // clients were told to run its client scripts
    // Files whose change triggers a reload, with the time we last saw.
    std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> watched;

    // Client side.
    std::unordered_set<net::NetId> tinted; // entities this resource coloured
};

struct HostLimits {
    double max_call_seconds = 0.25;
    double slow_call_seconds = 0.005;
    std::size_t memory_limit_bytes = 64u * 1024u * 1024u;
};

// Pushes a call's arguments onto the Lua stack and returns how many, or -1
// if the call should not happen after all.
using PushArguments = std::function<int(lua_State*)>;

class HostBase {
public:
    virtual ~HostBase() = default;

    // The clock timers run on: simulated seconds since the host started.
    double time() const { return time_; }

    // "server" or "client", for log lines.
    virtual const char* side() const = 0;

    // The engine events scripts may listen to on this side.
    virtual bool known_event(std::string_view event) const = 0;

protected:
    explicit HostBase(HostLimits limits);

    // Creates the resource's VM, installs the API and runs the scripts in
    // order. On any failure the resource is shut down and marked as failed.
    bool run_scripts(Resource& resource, const std::vector<net::ScriptFile>& scripts);

    // Marks a resource as failed and removes what it left in the world.
    void fail(Resource& resource, std::string error);

    // Frees the VM and removes what the resource left in the world.
    void shut_down(Resource& resource);

    // Calls every handler of an engine event.
    void dispatch(Resource& resource, const char* event, const PushArguments& push_arguments = {});

    // Calls every handler of a network event.
    void dispatch_net(Resource& resource, const std::string& event, const PushArguments& push_arguments);

    void run_timers(Resource& resource);

    // Adds this side's functions to the urt table, which is on top of the stack.
    virtual void register_side_api(Resource& resource, lua_State* L) = 0;

    // Removes what a resource put into the world: entities, colours, ...
    virtual void release_world(Resource& resource) = 0;

    // Called after a running resource failed, so others can be told.
    virtual void on_failed(Resource& resource) = 0;

    HostLimits limits_;
    double time_ = 0.0;

private:
    bool invoke(Resource& resource, int handler, std::string_view what, const PushArguments& push_arguments);
    void call_handlers(Resource& resource, const std::vector<int>& handlers, const std::string& what,
                       const PushArguments& push_arguments);
};

// --- helpers for functions called from Lua ---------------------------------------

// The resource a urt.* function was called from.
Resource& resource_of(lua_State* L);

// Adds a function to the table on top of the stack, bound to `resource`.
void add_function(lua_State* L, Resource& resource, const char* name, int (*function)(lua_State*));

// Reads a number argument and rejects NaN, infinity and anything beyond
// +-limit, with a script error naming `what`.
double check_number(lua_State* L, int index, double limit, const char* what);

// Reads an entity id argument: a positive whole number.
net::NetId check_entity_id(lua_State* L, int index);

// Reads an optional numeric field such as { x = 3 }. Missing means 0.
double optional_field(lua_State* L, int table, const char* field, double limit);

} // namespace urt::script
