#include "urt/net/protocol.hpp"

#include <algorithm>
#include <utility>

namespace urt::net {
namespace {

// Bytes one SnapshotEntity takes on the wire: id + position + yaw + velocity.
constexpr std::size_t snapshot_entity_size = 4 + 12 + 4 + 12;

void write_type(ByteWriter& out, MessageType type) { out.u8(static_cast<std::uint8_t>(type)); }

// Reads the type byte and checks that it is the expected message.
bool begin(ByteReader& reader, MessageType expected) {
    return static_cast<MessageType>(reader.u8()) == expected && reader.ok();
}

void write_vec3(ByteWriter& out, const glm::vec3& v) {
    out.f32(v.x);
    out.f32(v.y);
    out.f32(v.z);
}

glm::vec3 read_vec3(ByteReader& reader) {
    const float x = reader.f32();
    const float y = reader.f32();
    const float z = reader.f32();
    return {x, y, z};
}

} // namespace

MessageType message_type(std::span<const std::uint8_t> packet) {
    if (packet.empty()) {
        return MessageType::Invalid;
    }
    switch (static_cast<MessageType>(packet[0])) {
    case MessageType::ClientHello:
    case MessageType::ServerWelcome:
    case MessageType::ServerReject:
    case MessageType::ClientInput:
    case MessageType::ServerSnapshot:
        return static_cast<MessageType>(packet[0]);
    case MessageType::Invalid:
        break;
    }
    return MessageType::Invalid;
}

void encode(ByteWriter& out, const ClientHello& message) {
    write_type(out, MessageType::ClientHello);
    out.u16(message.version);
    out.string(std::string_view(message.name).substr(0, max_name_length));
}

bool decode(std::span<const std::uint8_t> packet, ClientHello& out) {
    ByteReader reader(packet);
    if (!begin(reader, MessageType::ClientHello)) {
        return false;
    }
    ClientHello message;
    message.version = reader.u16();
    // A client with a different version may lay out the rest differently.
    // Report the version so the server can answer with a proper reject.
    if (reader.ok() && message.version != protocol_version) {
        out = std::move(message);
        return true;
    }
    message.name = reader.string(max_name_length);
    if (!reader.finished()) {
        return false;
    }
    out = std::move(message);
    return true;
}

void encode(ByteWriter& out, const ServerWelcome& message) {
    write_type(out, MessageType::ServerWelcome);
    out.u32(message.player_id);
    out.u16(message.tick_rate);
    out.u32(message.server_tick);
}

bool decode(std::span<const std::uint8_t> packet, ServerWelcome& out) {
    ByteReader reader(packet);
    if (!begin(reader, MessageType::ServerWelcome)) {
        return false;
    }
    ServerWelcome message;
    message.player_id = reader.u32();
    message.tick_rate = reader.u16();
    message.server_tick = reader.u32();
    if (!reader.finished() || message.player_id == 0 || message.tick_rate == 0) {
        return false;
    }
    out = message;
    return true;
}

void encode(ByteWriter& out, const ServerReject& message) {
    write_type(out, MessageType::ServerReject);
    out.u8(static_cast<std::uint8_t>(message.reason));
}

bool decode(std::span<const std::uint8_t> packet, ServerReject& out) {
    ByteReader reader(packet);
    if (!begin(reader, MessageType::ServerReject)) {
        return false;
    }
    const std::uint8_t reason = reader.u8();
    if (!reader.finished() || reason < 1 || reason > 3) {
        return false;
    }
    out.reason = static_cast<RejectReason>(reason);
    return true;
}

void encode(ByteWriter& out, const ClientInput& message) {
    write_type(out, MessageType::ClientInput);
    out.u32(message.sequence);
    out.f32(message.move_x);
    out.f32(message.move_z);
    out.f32(message.yaw);
}

bool decode(std::span<const std::uint8_t> packet, ClientInput& out) {
    ByteReader reader(packet);
    if (!begin(reader, MessageType::ClientInput)) {
        return false;
    }
    ClientInput message;
    message.sequence = reader.u32();
    message.move_x = reader.f32();
    message.move_z = reader.f32();
    message.yaw = reader.f32();
    if (!reader.finished()) {
        return false;
    }
    out = message;
    return true;
}

void encode(ByteWriter& out, const ServerSnapshot& message) {
    const std::size_t count = std::min(message.entities.size(), max_snapshot_entities);
    write_type(out, MessageType::ServerSnapshot);
    out.u32(message.server_tick);
    out.u32(message.last_input_sequence);
    out.u16(static_cast<std::uint16_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        const SnapshotEntity& entity = message.entities[i];
        out.u32(entity.id);
        write_vec3(out, entity.position);
        out.f32(entity.yaw);
        write_vec3(out, entity.velocity);
    }
}

bool decode(std::span<const std::uint8_t> packet, ServerSnapshot& out) {
    ByteReader reader(packet);
    if (!begin(reader, MessageType::ServerSnapshot)) {
        return false;
    }
    ServerSnapshot message;
    message.server_tick = reader.u32();
    message.last_input_sequence = reader.u32();
    const std::size_t count = reader.u16();

    // Check the announced count against the real size BEFORE reserving memory.
    // Otherwise a 9-byte packet claiming 65535 entities would make us allocate
    // megabytes for nothing.
    if (!reader.ok() || count > max_snapshot_entities ||
        reader.remaining() != count * snapshot_entity_size) {
        return false;
    }

    message.entities.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        SnapshotEntity entity;
        entity.id = reader.u32();
        entity.position = read_vec3(reader);
        entity.yaw = reader.f32();
        entity.velocity = read_vec3(reader);
        if (entity.id == 0) {
            return false;
        }
        message.entities.push_back(entity);
    }
    if (!reader.finished()) {
        return false;
    }
    out = std::move(message);
    return true;
}

} // namespace urt::net
