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

bool NetClient::connect(const std::string& host, uint16_t port)
{
    const int sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == kInvalidSocket)
    {
        std::fprintf(stderr, "haven: socket() failed: %d\n", ::WSAGetLastError());
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
    {
        std::fprintf(stderr, "haven: inet_pton('%s') failed\n", host.c_str());
        ::closesocket(sock);
        return false;
    }

    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        std::fprintf(stderr, "haven: connect(%s:%u) failed: %d\n", host.c_str(), static_cast<unsigned>(port),
                     ::WSAGetLastError());
        ::closesocket(sock);
        return false;
    }

    // Loopback traffic is tiny and latency sensitive; Nagle only adds delay.
    BOOL no_delay = TRUE;
    ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));

    u_long non_blocking = 1;
    ::ioctlsocket(sock, FIONBIO, &non_blocking);

    socket_.store(sock);
    connected_.store(true);
    return true;
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
        outbound_.pop_front();
        send_dropped_.fetch_add(1);
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
        std::vector<uint8_t> packet;
        {
            std::lock_guard<std::mutex> lock(send_mutex_);
            if (outbound_.empty()) return true;
            packet = std::move(outbound_.front());
            outbound_.pop_front();
        }

        size_t sent = 0;
        while (sent < packet.size())
        {
            const int rc = ::send(sock, reinterpret_cast<const char*>(packet.data()) + sent,
                                  static_cast<int>(packet.size() - sent), 0);
            if (rc > 0)
            {
                sent += static_cast<size_t>(rc);
                continue;
            }
            if (rc == 0) return false;
            if (::WSAGetLastError() != WSAEWOULDBLOCK) return false;

            // Would block. Put the remainder back at the front so ordering is
            // preserved, and let the next writable pass finish the job.
            std::vector<uint8_t> rest(packet.begin() + static_cast<std::ptrdiff_t>(sent), packet.end());
            std::lock_guard<std::mutex> lock(send_mutex_);
            outbound_.push_front(std::move(rest));
            return true;
        }
    }
}

}  // namespace haven
