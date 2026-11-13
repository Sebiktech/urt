#include "urt/net/transport.hpp"

#include "urt/core/log.hpp"

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <enet/enet.h>

namespace urt::net {
namespace {

constexpr std::size_t channel_count = 2;

// ENet has to be initialised once per process (on Windows this starts
// Winsock). Count the users so several Transports can coexist.
std::mutex g_enet_mutex;
int g_enet_users = 0;

bool acquire_enet() {
    std::lock_guard lock(g_enet_mutex);
    if (g_enet_users == 0 && enet_initialize() != 0) {
        log::error("net", "failed to initialise the network library");
        return false;
    }
    ++g_enet_users;
    return true;
}

void release_enet() {
    std::lock_guard lock(g_enet_mutex);
    if (--g_enet_users == 0) {
        enet_deinitialize();
    }
}

// ENet lets us attach one pointer-sized value to each peer. We keep our
// PeerId there so an ENet event can be mapped back to our id.
PeerId id_of(const ENetPeer* peer) {
    return static_cast<PeerId>(reinterpret_cast<std::uintptr_t>(peer->data));
}

void set_id(ENetPeer* peer, PeerId id) {
    peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
}

Event connection_event(Event::Type type, PeerId peer) {
    Event event;
    event.type = type;
    event.peer = peer;
    return event;
}

} // namespace

struct Transport::Impl {
    bool enet_acquired = false;
    ENetHost* host = nullptr;
    bool listening = false;
    PeerId next_id = 1;
    std::unordered_map<PeerId, ENetPeer*> peers;

    PeerId add_peer(ENetPeer* peer) {
        const PeerId id = next_id++;
        set_id(peer, id);
        peers.emplace(id, peer);
        // Give up on a silent peer after 5 to 8 seconds instead of ENet's
        // default of up to 30. A connection attempt nobody answers fails
        // after about 7.5 seconds: ENet retries after 0.5, 1, 2 and 4 seconds
        // and checks these limits when a retry is due.
        enet_peer_timeout(peer, 8, 5000, 8000);
        return id;
    }

    void destroy_host() {
        if (host == nullptr) {
            return;
        }
        // disconnect_now tells the peer once and does not wait for an answer.
        for (auto& [id, peer] : peers) {
            enet_peer_disconnect_now(peer, 0);
        }
        peers.clear();
        enet_host_destroy(host);
        host = nullptr;
        listening = false;
    }
};

Transport::Transport() : impl_(std::make_unique<Impl>()) {
    impl_->enet_acquired = acquire_enet();
}

Transport::~Transport() {
    impl_->destroy_host();
    if (impl_->enet_acquired) {
        release_enet();
    }
}

bool Transport::listen(std::uint16_t port, std::size_t max_peers) {
    if (!impl_->enet_acquired || impl_->host != nullptr) {
        return false;
    }
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;
    impl_->host = enet_host_create(&address, max_peers, channel_count, 0, 0);
    if (impl_->host == nullptr) {
        log::error("net", "cannot listen on UDP port {} (is it already in use?)", port);
        return false;
    }
    impl_->listening = true;
    return true;
}

PeerId Transport::connect(const std::string& host, std::uint16_t port) {
    if (!impl_->enet_acquired || impl_->host != nullptr) {
        return 0;
    }
    ENetAddress address{};
    if (enet_address_set_host(&address, host.c_str()) != 0) {
        log::error("net", "cannot resolve host '{}'", host);
        return 0;
    }
    address.port = port;

    // A client host: no listening address, exactly one outgoing connection.
    impl_->host = enet_host_create(nullptr, 1, channel_count, 0, 0);
    if (impl_->host == nullptr) {
        log::error("net", "cannot create a network socket");
        return 0;
    }
    ENetPeer* peer = enet_host_connect(impl_->host, &address, channel_count, 0);
    if (peer == nullptr) {
        impl_->destroy_host();
        return 0;
    }
    return impl_->add_peer(peer);
}

std::uint16_t Transport::port() const {
    return impl_->host != nullptr && impl_->listening ? impl_->host->address.port : std::uint16_t{0};
}

bool Transport::is_open() const { return impl_->host != nullptr; }

void Transport::poll(std::vector<Event>& events) {
    if (impl_->host == nullptr) {
        return;
    }

    ENetEvent enet_event{};
    // Timeout 0: return immediately when there is nothing more to report.
    while (enet_host_service(impl_->host, &enet_event, 0) > 0) {
        switch (enet_event.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            // Outgoing connections already got their id in connect().
            PeerId id = id_of(enet_event.peer);
            if (id == 0) {
                id = impl_->add_peer(enet_event.peer);
            }
            events.push_back(connection_event(Event::Type::Connected, id));
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
            const PeerId id = id_of(enet_event.peer);
            set_id(enet_event.peer, 0);
            if (impl_->peers.erase(id) > 0) {
                events.push_back(connection_event(Event::Type::Disconnected, id));
            }
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE: {
            const PeerId id = id_of(enet_event.peer);
            const ENetPacket* packet = enet_event.packet;
            if (id != 0 && enet_event.channelID < channel_count) {
                Event event;
                event.type = Event::Type::Received;
                event.peer = id;
                event.channel = static_cast<Channel>(enet_event.channelID);
                event.data.assign(packet->data, packet->data + packet->dataLength);
                events.push_back(std::move(event));
            }
            // ENet hands us ownership of received packets.
            enet_packet_destroy(enet_event.packet);
            break;
        }
        case ENET_EVENT_TYPE_NONE:
            break;
        }
    }
}

bool Transport::send(PeerId peer, Channel channel, std::span<const std::uint8_t> data) {
    const auto found = impl_->peers.find(peer);
    if (found == impl_->peers.end()) {
        return false;
    }
    // Unreliable packets bigger than one UDP datagram are split into pieces
    // that are still unreliable; without the flag ENet would silently make
    // them reliable.
    const enet_uint32 flags = channel == Channel::Reliable
                                  ? static_cast<enet_uint32>(ENET_PACKET_FLAG_RELIABLE)
                                  : static_cast<enet_uint32>(ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
    ENetPacket* packet = enet_packet_create(data.data(), data.size(), flags);
    if (packet == nullptr) {
        return false;
    }
    if (enet_peer_send(found->second, static_cast<enet_uint8>(channel), packet) != 0) {
        // On failure the packet is still ours to free.
        enet_packet_destroy(packet);
        return false;
    }
    return true;
}

void Transport::disconnect(PeerId peer) {
    const auto found = impl_->peers.find(peer);
    if (found != impl_->peers.end()) {
        enet_peer_disconnect_later(found->second, 0);
    }
}

void Transport::flush() {
    if (impl_->host != nullptr) {
        enet_host_flush(impl_->host);
    }
}

void Transport::close() { impl_->destroy_host(); }

} // namespace urt::net
