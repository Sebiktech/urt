#pragma once

// The URT network protocol: every message the client and the server exchange.
// docs/network.md describes it in prose; keep the two in sync.
//
// Each packet holds exactly one message and starts with a MessageType byte.
// decode() returns false for anything that is not a complete, valid message
// of the expected type, including packets with extra bytes at the end.

#include "urt/net/bytes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <glm/vec3.hpp>

namespace urt::net {

// Raised whenever a change makes old clients and new servers incompatible.
inline constexpr std::uint16_t protocol_version = 2;

inline constexpr std::uint16_t default_port = 30777;

inline constexpr std::size_t max_name_length = 32;

// How many input commands fit in one ClientInput packet.
inline constexpr std::size_t max_inputs_per_packet = 8;

// Upper limit of entities in one snapshot. At 32 bytes each that is 32 KB.
inline constexpr std::size_t max_snapshot_entities = 1024;

// Network id of an entity. The server hands them out; 0 means "none".
using NetId = std::uint32_t;

enum class MessageType : std::uint8_t {
    Invalid = 0,
    ClientHello = 1,
    ServerWelcome = 2,
    ServerReject = 3,
    ClientInput = 4,
    ServerSnapshot = 5,
};

// The first byte of a packet, or Invalid if the packet is empty or unknown.
MessageType message_type(std::span<const std::uint8_t> packet);

// Client -> server, reliable. The first thing a client sends.
struct ClientHello {
    std::uint16_t version = protocol_version;
    std::string name;
};

// Server -> client, reliable. The answer to an accepted ClientHello.
struct ServerWelcome {
    NetId player_id = 0;          // network id of the entity this client controls
    std::uint16_t tick_rate = 30; // server ticks per second; the client must tick at the same rate
    std::uint32_t server_tick = 0;
    float walk_speed = 4.0f;      // metres per second; the client needs it to predict its own movement
};

enum class RejectReason : std::uint8_t {
    VersionMismatch = 1,
    ServerFull = 2,
    BadName = 3,
};

// Server -> client, reliable. The server disconnects right after it.
struct ServerReject {
    RejectReason reason = RejectReason::VersionMismatch;
};

// What the player wants during one simulation tick.
// The client says what the player wants, never where the player is.
struct InputCommand {
    float move_x = 0.0f; // wanted movement in world space, each -1..1
    float move_z = 0.0f;
    float yaw = 0.0f;    // where the player is facing, radians
};

// Client -> server, unreliable, once per client tick.
// Every tick produces one command with the next sequence number. A packet
// carries the newest command and a few before it, oldest first: if a packet
// is lost, the next one still delivers its commands.
struct ClientInput {
    std::uint32_t sequence = 0; // sequence number of the last (newest) command
    std::uint8_t count = 0;     // commands in use, 1..max_inputs_per_packet
    std::array<InputCommand, max_inputs_per_packet> commands{};
};

struct SnapshotEntity {
    NetId id = 0;
    glm::vec3 position{0.0f};
    float yaw = 0.0f;
    glm::vec3 velocity{0.0f};
};

// Server -> client, unreliable. The complete state of every entity near the
// receiving player. An entity missing from a snapshot is gone or out of range.
struct ServerSnapshot {
    std::uint32_t server_tick = 0;
    // Sequence of the last input command the server has simulated for this
    // client. The player's position in this snapshot is the result of all
    // commands up to and including this one.
    std::uint32_t last_input_sequence = 0;
    std::vector<SnapshotEntity> entities;
};

void encode(ByteWriter& out, const ClientHello& message);
void encode(ByteWriter& out, const ServerWelcome& message);
void encode(ByteWriter& out, const ServerReject& message);
void encode(ByteWriter& out, const ClientInput& message);
// Entities beyond max_snapshot_entities are not written.
void encode(ByteWriter& out, const ServerSnapshot& message);

bool decode(std::span<const std::uint8_t> packet, ClientHello& out);
bool decode(std::span<const std::uint8_t> packet, ServerWelcome& out);
bool decode(std::span<const std::uint8_t> packet, ServerReject& out);
bool decode(std::span<const std::uint8_t> packet, ClientInput& out);
bool decode(std::span<const std::uint8_t> packet, ServerSnapshot& out);

// True if `a` is newer than `b`. Sequence numbers and ticks are 32-bit and
// eventually wrap around to zero; comparing the difference keeps working then.
constexpr bool sequence_newer(std::uint32_t a, std::uint32_t b) {
    return static_cast<std::int32_t>(a - b) > 0;
}

} // namespace urt::net
