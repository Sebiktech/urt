#include <doctest/doctest.h>

#include "urt/net/bytes.hpp"
#include "urt/net/protocol.hpp"

#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace urt::net;

namespace {

std::vector<std::uint8_t> copy(const ByteWriter& writer) {
    return {writer.bytes().begin(), writer.bytes().end()};
}

template <class Message>
std::vector<std::uint8_t> encoded(const Message& message) {
    ByteWriter writer;
    encode(writer, message);
    return copy(writer);
}

ServerSnapshot sample_snapshot(std::size_t entity_count) {
    ServerSnapshot snapshot;
    snapshot.server_tick = 123456;
    snapshot.last_input_sequence = 77;
    for (std::size_t i = 0; i < entity_count; ++i) {
        const auto f = static_cast<float>(i);
        snapshot.entities.push_back(SnapshotEntity{
            .id = static_cast<NetId>(i + 1),
            .position = {f, -f * 0.5f, f * 100.0f},
            .yaw = f * 0.1f,
            .velocity = {1.0f, 0.0f, -f},
        });
    }
    return snapshot;
}

} // namespace

TEST_CASE("ByteWriter and ByteReader round-trip values in little-endian") {
    ByteWriter writer;
    writer.u8(0xAB);
    writer.u16(0x1234);
    writer.u32(0xDEADBEEF);
    writer.f32(-12.5f);
    writer.string("URT");

    const std::vector<std::uint8_t> bytes = copy(writer);
    // Spot-check the layout: low byte first.
    CHECK(bytes[1] == 0x34);
    CHECK(bytes[2] == 0x12);
    CHECK(bytes[3] == 0xEF);

    ByteReader reader(bytes);
    CHECK(reader.u8() == 0xAB);
    CHECK(reader.u16() == 0x1234);
    CHECK(reader.u32() == 0xDEADBEEF);
    CHECK(reader.f32() == -12.5f);
    CHECK(reader.string(8) == "URT");
    CHECK(reader.finished());
}

TEST_CASE("ByteReader fails safely instead of reading past the end") {
    const std::vector<std::uint8_t> bytes{1, 2, 3};
    ByteReader reader(bytes);

    CHECK(reader.u16() == 0x0201);
    CHECK(reader.ok());
    CHECK(reader.u32() == 0); // only one byte left
    CHECK_FALSE(reader.ok());
    CHECK(reader.u8() == 0); // stays failed, even though a byte is left
    CHECK_FALSE(reader.finished());
}

TEST_CASE("ByteReader rejects NaN, infinity and oversized strings") {
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity()}) {
        ByteWriter writer;
        writer.f32(bad);
        ByteReader reader(writer.bytes());
        CHECK(reader.f32() == 0.0f);
        CHECK_FALSE(reader.ok());
    }

    ByteWriter writer;
    writer.string("too long");
    ByteReader limited(writer.bytes());
    CHECK(limited.string(4).empty());
    CHECK_FALSE(limited.ok());

    // A length byte that promises more than the packet holds.
    const std::vector<std::uint8_t> lying{200, 'a', 'b'};
    ByteReader reader(lying);
    CHECK(reader.string(255).empty());
    CHECK_FALSE(reader.ok());
}

TEST_CASE("ByteWriter cuts strings to 255 bytes") {
    ByteWriter writer;
    writer.string(std::string(300, 'x'));
    CHECK(writer.size() == 256);
    ByteReader reader(writer.bytes());
    CHECK(reader.string(255).size() == 255);
    CHECK(reader.finished());
}

TEST_CASE("every message survives encode and decode") {
    SUBCASE("ClientHello") {
        ClientHello out;
        REQUIRE(decode(encoded(ClientHello{.version = protocol_version, .name = "Sebastián"}), out));
        CHECK(out.version == protocol_version);
        CHECK(out.name == "Sebastián");
    }
    SUBCASE("ServerWelcome") {
        ServerWelcome out;
        REQUIRE(decode(encoded(ServerWelcome{.player_id = 42, .tick_rate = 60, .server_tick = 9000}), out));
        CHECK(out.player_id == 42);
        CHECK(out.tick_rate == 60);
        CHECK(out.server_tick == 9000);
    }
    SUBCASE("ServerReject") {
        ServerReject out;
        REQUIRE(decode(encoded(ServerReject{.reason = RejectReason::ServerFull}), out));
        CHECK(out.reason == RejectReason::ServerFull);
    }
    SUBCASE("ClientInput") {
        ClientInput out;
        REQUIRE(decode(encoded(ClientInput{.sequence = 5, .move_x = -1.0f, .move_z = 0.25f, .yaw = 3.0f}), out));
        CHECK(out.sequence == 5);
        CHECK(out.move_x == -1.0f);
        CHECK(out.move_z == 0.25f);
        CHECK(out.yaw == 3.0f);
    }
    SUBCASE("ServerSnapshot") {
        for (const std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{300}}) {
            const ServerSnapshot in = sample_snapshot(count);
            ServerSnapshot out;
            REQUIRE(decode(encoded(in), out));
            CHECK(out.server_tick == in.server_tick);
            CHECK(out.last_input_sequence == in.last_input_sequence);
            REQUIRE(out.entities.size() == count);
            for (std::size_t i = 0; i < count; ++i) {
                CHECK(out.entities[i].id == in.entities[i].id);
                CHECK(out.entities[i].position == in.entities[i].position);
                CHECK(out.entities[i].yaw == in.entities[i].yaw);
                CHECK(out.entities[i].velocity == in.entities[i].velocity);
            }
        }
    }
}

TEST_CASE("decode rejects the wrong type, truncated packets and trailing bytes") {
    const std::vector<std::uint8_t> input = encoded(ClientInput{.sequence = 1});
    const std::vector<std::uint8_t> snapshot = encoded(sample_snapshot(3));

    ClientInput input_out;
    ServerSnapshot snapshot_out;
    ClientHello hello_out;

    CHECK_FALSE(decode(input, snapshot_out)); // an input is not a snapshot
    CHECK_FALSE(decode(snapshot, input_out));
    CHECK_FALSE(decode(std::vector<std::uint8_t>{}, input_out));

    // Every shorter prefix of a valid packet must be refused.
    for (std::size_t length = 0; length < input.size(); ++length) {
        const std::vector<std::uint8_t> cut(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(length));
        CHECK_FALSE(decode(cut, input_out));
    }
    for (std::size_t length = 0; length < snapshot.size(); ++length) {
        const std::vector<std::uint8_t> cut(snapshot.begin(),
                                            snapshot.begin() + static_cast<std::ptrdiff_t>(length));
        CHECK_FALSE(decode(cut, snapshot_out));
    }

    std::vector<std::uint8_t> padded = input;
    padded.push_back(0);
    CHECK_FALSE(decode(padded, input_out));

    // A hello whose name is longer than allowed.
    ByteWriter writer;
    writer.u8(static_cast<std::uint8_t>(MessageType::ClientHello));
    writer.u16(protocol_version);
    writer.string(std::string(max_name_length + 1, 'a'));
    CHECK_FALSE(decode(writer.bytes(), hello_out));
}

TEST_CASE("a hello from another protocol version still reports its version") {
    // The server needs the number to answer "version mismatch", even though
    // it cannot understand the rest of the packet.
    ByteWriter writer;
    writer.u8(static_cast<std::uint8_t>(MessageType::ClientHello));
    writer.u16(static_cast<std::uint16_t>(protocol_version + 1));
    writer.u32(0xFFFFFFFF); // fields we know nothing about

    ClientHello out;
    REQUIRE(decode(writer.bytes(), out));
    CHECK(out.version == protocol_version + 1);
}

TEST_CASE("a snapshot cannot claim more entities than it carries") {
    ByteWriter writer;
    writer.u8(static_cast<std::uint8_t>(MessageType::ServerSnapshot));
    writer.u32(1);
    writer.u32(1);
    writer.u16(65535); // a lie: no entity data follows

    ServerSnapshot out;
    CHECK_FALSE(decode(writer.bytes(), out));
    CHECK(out.entities.empty());

    // Entity id 0 is reserved and never valid.
    ServerSnapshot zero = sample_snapshot(2);
    zero.entities[1].id = 0;
    CHECK_FALSE(decode(encoded(zero), out));

    // NaN inside an entity.
    ServerSnapshot poisoned = sample_snapshot(2);
    poisoned.entities[0].position.x = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(decode(encoded(poisoned), out));
}

TEST_CASE("encode never writes more than max_snapshot_entities") {
    const ServerSnapshot big = sample_snapshot(max_snapshot_entities + 50);
    ServerSnapshot out;
    REQUIRE(decode(encoded(big), out));
    CHECK(out.entities.size() == max_snapshot_entities);
}

TEST_CASE("decoders survive random garbage") {
    // Nothing here checks results: the test passes if nothing crashes or
    // reads out of bounds. The sanitizer builds are what make it meaningful.
    std::mt19937 random(12345);
    std::uniform_int_distribution<int> byte(0, 255);
    std::uniform_int_distribution<int> length(0, 96);
    std::uniform_int_distribution<int> type(0, 6);

    ClientHello hello;
    ServerWelcome welcome;
    ServerReject reject;
    ClientInput input;
    ServerSnapshot snapshot;

    for (int round = 0; round < 20000; ++round) {
        std::vector<std::uint8_t> packet(static_cast<std::size_t>(length(random)));
        for (std::uint8_t& value : packet) {
            value = static_cast<std::uint8_t>(byte(random));
        }
        if (!packet.empty()) {
            packet[0] = static_cast<std::uint8_t>(type(random)); // mostly valid type bytes
        }
        (void)message_type(packet);
        (void)decode(packet, hello);
        (void)decode(packet, welcome);
        (void)decode(packet, reject);
        (void)decode(packet, input);
        (void)decode(packet, snapshot);
    }
    CHECK(true);
}

TEST_CASE("sequence_newer handles wrap-around") {
    CHECK(sequence_newer(2, 1));
    CHECK_FALSE(sequence_newer(1, 2));
    CHECK_FALSE(sequence_newer(7, 7));
    CHECK(sequence_newer(3, 0xFFFFFFF0u)); // wrapped past zero
    CHECK_FALSE(sequence_newer(0xFFFFFFF0u, 3));
}
