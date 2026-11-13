#pragma once

// Client-side scripting: runs the scripts the server sends.
//
// When a client joins, the server sends the client scripts of its resources;
// when a resource is reloaded on the server, it sends them again. They run
// here in the same kind of sandbox as on the server, with a smaller API:
// client scripts can look at the world, colour entities, and exchange events
// with the server's scripts, but they cannot change the game state. That
// stays with the server.
//
// The server is not trusted. Its scripts have no access to this computer
// beyond the urt.* functions, and their running time and memory are limited
// exactly as described in docs/scripting.md.
//
// Add this system after net::Client.

#include "urt/ecs/system.hpp"
#include "urt/ecs/world.hpp"
#include "urt/script/script_system.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace urt::net {
class Client;
}

namespace urt::script {

struct ClientScriptConfig {
    double max_call_seconds = 0.25;  // see ScriptConfig
    double slow_call_seconds = 0.005;
    std::size_t memory_limit_bytes = 64u * 1024u * 1024u;
    std::size_t max_resources = 128; // more than this from one server is refused
};

class ClientScriptSystem final : public System {
public:
    explicit ClientScriptSystem(net::Client& client, ClientScriptConfig config = {});
    ~ClientScriptSystem() override;

    std::string_view name() const override { return "client-script"; }

    void tick(World& world, const TickContext& context) override;

    std::vector<ResourceInfo> resources() const;
    std::optional<ResourceInfo> resource(std::string_view name) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace urt::script
