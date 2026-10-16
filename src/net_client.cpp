#include "net_client.h"

#include "protocol.h"

#include <ws2tcpip.h>

#include <cstdio>

namespace haven {

NetClient::NetClient() = default;

NetClient::~NetClient()
{
    stop();
    close_socket();
}

bool NetClient::connect(const std::string& host, uint16_t port)
{
    socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == INVALID_SOCKET)
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
        close_socket();
        return false;
    }

    if (::connect(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        std::fprintf(stderr, "haven: connect(%s:%u) failed: %d\n", host.c_str(), static_cast<unsigned>(port),
                     ::WSAGetLastError());
        close_socket();
        return false;
    }

    // Loopback traffic is tiny and latency sensitive; Nagle only adds delay.
    BOOL no_delay = TRUE;
    ::setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));

    u_long non_blocking = 1;
    ::ioctlsocket(socket_, FIONBIO, &non_blocking);

    connected_.store(true);
    return true;
}

void NetClient::start()
{
    if (running_.exchange(true)) return;
    reader_ = std::thread([this] { run(); });
}

void NetClient::stop()
{
    if (!running_.exchange(false)) return;
    close_socket();
    if (reader_.joinable()) reader_.join();
}

bool NetClient::send(const std::vector<uint8_t>& packet)
{
    if (!connected_.load() || socket_ == INVALID_SOCKET) return false;
    std::lock_guard<std::mutex> lock(send_mutex_);
    return write_all(packet.data(), packet.size());
}

void NetClient::poll(std::vector<std::vector<uint8_t>>& out)
{
    std::lock_guard<std::mutex> lock(recv_mutex_);
    out.insert(out.end(), std::make_move_iterator(inbound_.begin()), std::make_move_iterator(inbound_.end()));
    inbound_.clear();
}

void NetClient::run()
{
    std::vector<uint8_t> header(kHeaderSize);
    std::vector<uint8_t> payload;

    while (running_.load())
    {
        if (!wait_readable(100)) continue;
        if (!read_exact(header.data(), kHeaderSize)) break;

        PacketHeader parsed{};
        if (!decode_header(header.data(), kHeaderSize, parsed)) break;

        payload.resize(parsed.payload_len);
        if (parsed.payload_len > 0 && !read_exact(payload.data(), parsed.payload_len)) break;

        std::vector<uint8_t> packet;
        packet.reserve(kHeaderSize + payload.size());
        packet.insert(packet.end(), header.begin(), header.end());
        packet.insert(packet.end(), payload.begin(), payload.end());

        {
            std::lock_guard<std::mutex> lock(recv_mutex_);
            inbound_.push_back(std::move(packet));
        }
    }

    connected_.store(false);
    std::fprintf(stderr, "haven: socket closed\n");
}

bool NetClient::wait_readable(unsigned timeout_ms) const
{
    if (socket_ == INVALID_SOCKET) return false;
    fd_set read{};
    FD_ZERO(&read);
    FD_SET(socket_, &read);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout_ms / 1000);
    tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
    const int rc = ::select(0, &read, nullptr, nullptr, &tv);
    return rc > 0;
}

bool NetClient::wait_writable(unsigned timeout_ms) const
{
    if (socket_ == INVALID_SOCKET) return false;
    fd_set write{};
    FD_ZERO(&write);
    FD_SET(socket_, &write);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout_ms / 1000);
    tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
    const int rc = ::select(0, nullptr, &write, nullptr, &tv);
    return rc > 0;
}

bool NetClient::read_exact(uint8_t* dst, size_t len)
{
    size_t got = 0;
    while (got < len)
    {
        if (!wait_readable(5000)) return false;
        const int rc = ::recv(socket_, reinterpret_cast<char*>(dst) + got, static_cast<int>(len - got), 0);
        if (rc == 0) return false;            // peer closed
        if (rc == SOCKET_ERROR)
        {
            const int err = ::WSAGetLastError();
            if (err == WSAEWOULDBLOCK) continue;
            return false;
        }
        got += static_cast<size_t>(rc);
    }
    return true;
}

bool NetClient::write_all(const uint8_t* src, size_t len)
{
    size_t sent = 0;
    while (sent < len)
    {
        if (!wait_writable(5000)) return false;
        const int rc = ::send(socket_, reinterpret_cast<const char*>(src) + sent, static_cast<int>(len - sent), 0);
        if (rc == SOCKET_ERROR)
        {
            const int err = ::WSAGetLastError();
            if (err == WSAEWOULDBLOCK) continue;
            return false;
        }
        sent += static_cast<size_t>(rc);
    }
    return true;
}

void NetClient::close_socket()
{
    if (socket_ == INVALID_SOCKET) return;
    ::closesocket(socket_);
    socket_ = INVALID_SOCKET;
    connected_.store(false);
}

}  // namespace haven
