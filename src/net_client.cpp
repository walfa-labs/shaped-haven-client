#include "net_client.h"

#include "protocol.h"

// windows.h (pulled in by ws2tcpip.h) defines min/max as macros, which breaks
// std::min below.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <ws2tcpip.h>

#include <algorithm>
#include <cstdio>

namespace haven {

NetClient::NetClient() = default;

NetClient::~NetClient()
{
    stop();
    // Only reachable when no socket thread was ever started, so the handle is
    // still ours; the thread closes its own on the way out.
    const int sock = socket_.exchange(kInvalidSocket);
    if (sock != kInvalidSocket) ::closesocket(sock);
}

void NetClient::adopt(int sock)
{
    socket_.store(sock);
    connected_.store(true);
    start();
}

void NetClient::start()
{
    if (socket_thread_.joinable()) return;
    running_.store(true);
    socket_thread_ = std::thread([this] { run(); });
}

void NetClient::stop()
{
    running_.store(false);
    // joinable() is the real "is there a thread to wait for" test. running_
    // being false also happens once the peer has gone away on its own, which
    // must not skip the join.
    if (socket_thread_.joinable()) socket_thread_.join();
}

bool NetClient::send(const std::vector<uint8_t>& packet)
{
    if (!connected_.load() || socket_.load() == kInvalidSocket) return false;

    std::lock_guard<std::mutex> lock(send_mutex_);
    if (outbound_.size() >= kMaxOutgoingPackets)
    {
        // Refuse instead of evicting. The queue only ever holds whole packets,
        // so refusing cannot corrupt the stream, and returning false is what
        // tells the caller its input never reached the server.
        send_dropped_.fetch_add(1);
        return false;
    }
    outbound_.push_back(packet);
    return true;
}

void NetClient::poll(std::vector<std::vector<uint8_t>>& out, size_t max_packets)
{
    std::lock_guard<std::mutex> lock(recv_mutex_);
    const size_t available = inbound_.size();
    const size_t count = max_packets == 0 ? available : std::min(max_packets, available);
    auto begin = inbound_.begin();
    auto end = begin + static_cast<std::ptrdiff_t>(count);
    out.insert(out.end(), std::make_move_iterator(begin), std::make_move_iterator(end));
    inbound_.erase(begin, end);
}

void NetClient::run()
{
    // Ownership of the handle transfers here and is never shared again.
    const int sock = socket_.load();
    if (sock == kInvalidSocket)
    {
        connected_.store(false);
        return;
    }

    while (running_.load())
    {
        bool backlog_full = false;
        bool has_outgoing = false;
        {
            std::lock_guard<std::mutex> recv_lock(recv_mutex_);
            std::lock_guard<std::mutex> send_lock(send_mutex_);
            backlog_full = inbound_.size() >= kMaxIncomingPackets;
            has_outgoing = !outbound_.empty();
        }

        fd_set read{};
        FD_ZERO(&read);
        if (!backlog_full) FD_SET(sock, &read);
        fd_set write{};
        FD_ZERO(&write);
        if (has_outgoing) FD_SET(sock, &write);

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 50000;  // 50 ms, so stop() stays prompt without spinning
        const int rc = ::select(0, &read, has_outgoing ? &write : nullptr, nullptr, &tv);
        if (rc == SOCKET_ERROR) break;
        if (rc == 0) continue;

        if (!backlog_full && FD_ISSET(sock, &read))
        {
            std::vector<uint8_t> packet;
            const ReadStatus status = read_packet(sock, packet);
            if (status == ReadStatus::Failed) break;
            if (status == ReadStatus::Packet)
            {
                std::lock_guard<std::mutex> lock(recv_mutex_);
                inbound_.push_back(std::move(packet));
            }
        }

        if (has_outgoing && FD_ISSET(sock, &write))
        {
            if (!flush_outgoing(sock)) break;
        }
    }

    connected_.store(false);
    const int closed = socket_.exchange(kInvalidSocket);
    if (closed != kInvalidSocket) ::closesocket(closed);
    std::fprintf(stderr, "haven: socket closed\n");
}

NetClient::ReadStatus NetClient::read_packet(int sock, std::vector<uint8_t>& packet)
{
    // read_offset_ and read_phase_ persist across calls so a packet split over
    // several TCP segments is reassembled instead of restarted.
    for (;;)
    {
        if (read_phase_ == ReadPhase::Header)
        {
            const int rc = ::recv(sock, reinterpret_cast<char*>(header_.data()) + read_offset_,
                                  static_cast<int>(kHeaderSize - read_offset_), 0);
            if (rc > 0)
            {
                read_offset_ += static_cast<size_t>(rc);
                if (read_offset_ < kHeaderSize) continue;
                read_offset_ = 0;
                if (!decode_header(header_.data(), kHeaderSize, parsed_)) return ReadStatus::Failed;
                payload_.resize(parsed_.payload_len);
                if (parsed_.payload_len == 0)
                {
                    // The packet is already complete: a zero length recv() would
                    // return 0 and be misread as the peer closing.
                    read_phase_ = ReadPhase::Header;
                    packet.assign(header_.begin(), header_.end());
                    return ReadStatus::Packet;
                }
                read_phase_ = ReadPhase::Payload;
                continue;
            }
            if (rc == 0) return ReadStatus::Failed;
            if (::WSAGetLastError() == WSAEWOULDBLOCK) return ReadStatus::Again;
            return ReadStatus::Failed;
        }

        const int rc = ::recv(sock, reinterpret_cast<char*>(payload_.data()) + read_offset_,
                              static_cast<int>(payload_.size() - read_offset_), 0);
        if (rc > 0)
        {
            read_offset_ += static_cast<size_t>(rc);
            if (read_offset_ < payload_.size()) continue;
            read_offset_ = 0;
            read_phase_ = ReadPhase::Header;

            packet.reserve(kHeaderSize + payload_.size());
            packet.insert(packet.end(), header_.begin(), header_.end());
            packet.insert(packet.end(), payload_.begin(), payload_.end());
            return ReadStatus::Packet;
        }
        if (rc == 0) return ReadStatus::Failed;
        if (::WSAGetLastError() == WSAEWOULDBLOCK) return ReadStatus::Again;
        return ReadStatus::Failed;
    }
}

bool NetClient::flush_outgoing(int sock)
{
    for (;;)
    {
        // A packet that could not be sent in one go leaves its tail in
        // `send_remainder_`, which lives outside the queue so no queue policy
        // can drop it and splice two packets together on the wire.
        if (send_remainder_.empty())
        {
            std::lock_guard<std::mutex> lock(send_mutex_);
            if (outbound_.empty()) return true;
            send_remainder_ = std::move(outbound_.front());
            outbound_.pop_front();
        }

        while (!send_remainder_.empty())
        {
            const int rc = ::send(sock, reinterpret_cast<const char*>(send_remainder_.data()),
                                  static_cast<int>(send_remainder_.size()), 0);
            if (rc > 0)
            {
                send_remainder_.erase(send_remainder_.begin(),
                                      send_remainder_.begin() + static_cast<std::ptrdiff_t>(rc));
                continue;
            }
            if (rc == 0) return false;
            if (::WSAGetLastError() != WSAEWOULDBLOCK) return false;

            // Would block. The remainder stays put and the next writable pass
            // finishes the packet before starting another one.
            return true;
        }
    }
}

}  // namespace haven
