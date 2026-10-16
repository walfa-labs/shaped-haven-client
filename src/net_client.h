#pragma once

#include "protocol.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace haven {

/// Winsock's INVALID_SOCKET, spelled without pulling windows.h into this
/// header. Only the socket thread ever interprets it.
constexpr int kInvalidSocket = -1;

/// TCP socket driven from a single owner thread.
///
/// The socket thread is created by start() and is the only code that ever reads
/// from, writes to, or closes the handle. Everything the game thread does is
/// enqueue under a mutex and return, so the render loop can never block on the
/// socket — neither on a full send buffer nor on a stalled peer.
///
/// Packets are framed exactly as DESIGN.md section 5 specifies: a 7 byte header
/// (msg_type u8, payload_len u16, seq u32) followed by payload_len bytes.
class NetClient {
public:
    /// Outgoing queue capacity. On loopback this never fills; it exists so a
    /// stalled peer cannot grow memory without bound.
    static constexpr size_t kMaxOutgoingPackets = 64;
    /// Inbound backlog limit. Once reached the reader stops reading and lets the
    /// kernel receive buffer apply backpressure instead of dropping snapshots.
    static constexpr size_t kMaxIncomingPackets = 64;

    NetClient();
    ~NetClient();

    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    /// Synchronous connect; call before start(). Returns false on failure.
    bool connect(const std::string& host, uint16_t port);

    /// Spawns the socket thread and transfers ownership of the handle to it.
    void start();

    /// Signals the socket thread to stop and joins it. Safe to call more than
    /// once, and safe to call before start().
    void stop();

    /// True between a successful connect() and the socket thread exiting.
    bool connected() const { return connected_.load(); }

    /// Never blocks and never touches the socket on the calling thread.
    ///
    /// Appends to a bounded queue and returns true. When the queue is full the
    /// oldest queued packet is evicted to make room, because the newest input is
    /// the one that reflects what the player is doing now; send_dropped() counts
    /// those evictions. Returns false only when the link is down, in which case
    /// nothing is queued.
    bool send(const std::vector<uint8_t>& packet);

    /// Packets evicted from the outgoing queue because it was full.
    uint32_t send_dropped() const { return send_dropped_.load(); }

    /// Moves every complete packet received since the last call into `out`.
    /// `max_packets` caps how many are handed over in one call; 0 means all.
    void poll(std::vector<std::vector<uint8_t>>& out, size_t max_packets = 0);

private:
    enum class ReadStatus { Again, Packet, Failed };
    enum class ReadPhase { Header, Payload };

    void run();
    ReadStatus read_packet(int sock, std::vector<uint8_t>& packet);
    bool flush_outgoing(int sock);

    /// The handle is handed to the socket thread by start(). Both that thread
    /// and the destructor use exchange(), so exactly one of them closes it.
    std::atomic<int> socket_{kInvalidSocket};
    std::thread socket_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint32_t> send_dropped_{0};

    std::mutex send_mutex_;
    std::deque<std::vector<uint8_t>> outbound_;

    std::mutex recv_mutex_;
    std::deque<std::vector<uint8_t>> inbound_;

    // Receive reassembly state; only the socket thread touches these.
    //
    // `=` rather than `{}` or `()`: braces would select the initializer_list
    // constructor and build a two element vector (so the header recv would run
    // off the end of the allocation), and parentheses are not a valid default
    // member initialiser at all.
    std::vector<uint8_t> header_ = std::vector<uint8_t>(kHeaderSize, 0);
    std::vector<uint8_t> payload_;
    PacketHeader parsed_{};
    size_t read_offset_ = 0;
    ReadPhase read_phase_ = ReadPhase::Header;
};

}  // namespace haven
