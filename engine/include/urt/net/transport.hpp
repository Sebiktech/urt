#pragma once

// Transport: sends and receives packets over UDP.
//
// This is the only place that knows about ENet. The rest of the engine sees
// connections as PeerIds and packets as byte arrays, so the library can be
// replaced later without touching the protocol or the game.
//
// A Transport is used from one thread only (the tick thread).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace urt::net {

// Identifies one connection for the lifetime of a Transport. Never reused.
using PeerId = std::uint32_t;

enum class Channel : std::uint8_t {
    // Arrives exactly once and in order; resent until acknowledged.
    // For things that must not be lost: handshake, later chat and events.
    Reliable = 0,
    // May be lost; a packet older than one already received is dropped.
    // For state that is replaced every tick anyway: inputs and snapshots.
    Unreliable = 1,
};

struct Event {
    enum class Type { Connected, Disconnected, Received };

    Type type = Type::Received;
    PeerId peer = 0;
    Channel channel = Channel::Reliable; // only for Received
    std::vector<std::uint8_t> data;      // only for Received
};

class Transport {
public:
    Transport();
    ~Transport();

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    // Server: start accepting connections. Port 0 lets the OS pick a free one.
    bool listen(std::uint16_t port, std::size_t max_peers);

    // Client: start connecting. Returns the id the server will have in events,
    // or 0 if the address is invalid. The result arrives later as a Connected
    // or a Disconnected event.
    PeerId connect(const std::string& host, std::uint16_t port);

    // The local port after listen(), 0 otherwise.
    std::uint16_t port() const;

    bool is_open() const;

    // Collects everything that happened since the last call. Never blocks.
    void poll(std::vector<Event>& events);

    // Queues a packet. Returns false if the peer is unknown or sending failed.
    bool send(PeerId peer, Channel channel, std::span<const std::uint8_t> data);

    // Sends what is queued for the peer, then closes the connection.
    // A Disconnected event follows.
    void disconnect(PeerId peer);

    // Puts queued packets on the wire now instead of at the next poll().
    void flush();

    // Closes everything immediately. Connected peers are told once, without
    // waiting for an answer.
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace urt::net
