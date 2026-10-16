#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace haven {

/// Blocking TCP socket driven from its own thread. The game thread only ever
/// calls send() and poll(); the reader thread owns the socket.
///
/// Packets are framed exactly as DESIGN.md section 5 specifies: a 7 byte header
/// (msg_type u8, payload_len u16, seq u32) followed by payload_len bytes.
class NetClient {
public:
    NetClient();
    ~NetClient();

    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    /// Synchronous connect; call before start(). Returns false on failure.
    bool connect(const std::string& host, uint16_t port);

    /// Spawns the reader thread.
    void start();

    /// Signals the reader thread to stop and joins it.
    void stop();

    /// True between a successful connect() and the socket closing.
    bool connected() const { return connected_.load(); }

    /// Thread-safe, never blocks the caller for long.
    bool send(const std::vector<uint8_t>& packet);

    /// Moves every complete packet received since the last call into `out`.
    void poll(std::vector<std::vector<uint8_t>>& out);

private:
    void run();
    bool wait_readable(unsigned timeout_ms) const;
    bool wait_writable(unsigned timeout_ms) const;
    bool read_exact(uint8_t* dst, size_t len);
    bool write_all(const uint8_t* src, size_t len);
    void close_socket();

    int socket_ = -1;
    std::thread reader_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    std::mutex send_mutex_;
    std::mutex recv_mutex_;
    std::deque<std::vector<uint8_t>> inbound_;
};

}  // namespace haven
