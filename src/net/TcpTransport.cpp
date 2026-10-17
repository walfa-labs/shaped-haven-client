#include "net/TcpTransport.h"

// winsock2.h pulls in windows.h, which defines min/max as macros.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>

namespace haven {

/// How long a non-blocking connect may stay incomplete before the transport
/// gives up. Long enough for a server that is still starting, short enough
/// that the menu does not appear hung.
constexpr int kConnectTimeoutMs = 3000;

TcpTransport::~TcpTransport()
{
    disconnect();
}

bool TcpTransport::connect(const std::string& host, uint16_t port, std::string& outError)
{
    const int sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == kInvalidSocket)
    {
        outError = "socket() failed: " + std::to_string(::WSAGetLastError());
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
    {
        outError = "cannot parse host '" + host + "'";
        ::closesocket(sock);
        return false;
    }

    // Non-blocking so the timeout below is ours to keep rather than the
    // operating system's, which is far longer.
    u_long non_blocking = 1;
    ::ioctlsocket(sock, FIONBIO, &non_blocking);

    const int rc = ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == SOCKET_ERROR && ::WSAGetLastError() != WSAEWOULDBLOCK)
    {
        outError = "connect(" + host + ":" + std::to_string(static_cast<unsigned>(port)) +
                   ") failed: " + std::to_string(::WSAGetLastError());
        ::closesocket(sock);
        return false;
    }

    if (rc == SOCKET_ERROR)
    {
        // The connect is in progress. Wait for it to complete, one way or the
        // other. Windows signals a *failed* connect in the exception set and a
        // successful one in the write set, so both are watched; SO_ERROR then
        // says which happened.
        fd_set write{};
        FD_ZERO(&write);
        FD_SET(sock, &write);
        fd_set except{};
        FD_ZERO(&except);
        FD_SET(sock, &except);
        timeval tv{};
        tv.tv_sec = kConnectTimeoutMs / 1000;
        tv.tv_usec = (kConnectTimeoutMs % 1000) * 1000;
        const int ready = ::select(0, nullptr, &write, &except, &tv);
        if (ready == SOCKET_ERROR)
        {
            outError = "connect(" + host + ":" + std::to_string(static_cast<unsigned>(port)) +
                       ") failed: " + std::to_string(::WSAGetLastError());
            ::closesocket(sock);
            return false;
        }
        if (ready == 0)
        {
            outError = "connect(" + host + ":" + std::to_string(static_cast<unsigned>(port)) +
                       ") timed out after " + std::to_string(kConnectTimeoutMs) + " ms";
            ::closesocket(sock);
            return false;
        }

        // The connect completed. SO_ERROR is 0 on success and the reason on
        // failure, whichever set select() reported it through.
        int so_error = 0;
        int so_len = sizeof(so_error);
        if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &so_len) != 0 ||
            so_error != 0)
        {
            outError = "connect(" + host + ":" + std::to_string(static_cast<unsigned>(port)) +
                       ") failed: " + std::to_string(so_error);
            ::closesocket(sock);
            return false;
        }
    }

    // Loopback traffic is tiny and latency sensitive; Nagle only adds delay.
    BOOL no_delay = TRUE;
    ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));

    net_.adopt(sock);
    return true;
}

void TcpTransport::update(float /*dtSeconds*/)
{
    // NetClient drives the socket from its own thread, so the async I/O is
    // already pumped there and there is nothing to poll here. The method
    // exists so GameSession has one uniform per-frame call site for every
    // transport, including the ones that do pump.
}

bool TcpTransport::sendPacket(const uint8_t* data, size_t len)
{
    return net_.send(std::vector<uint8_t>(data, data + len));
}

size_t TcpTransport::receiveBytes(uint8_t* outBuf, size_t maxLen)
{
    if (maxLen == 0) return 0;

    // Finish the packet the caller's last buffer was too small for before
    // pulling a new one, so bytes are never reordered or dropped.
    if (!receive_remainder_.empty())
    {
        const size_t n = std::min(receive_remainder_.size(), maxLen);
        std::memcpy(outBuf, receive_remainder_.data(), n);
        receive_remainder_.erase(receive_remainder_.begin(),
                                 receive_remainder_.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }

    std::vector<std::vector<uint8_t>> packets;
    net_.poll(packets, 1);
    if (packets.empty()) return 0;

    const std::vector<uint8_t>& packet = packets.front();
    const size_t n = std::min(packet.size(), maxLen);
    std::memcpy(outBuf, packet.data(), n);
    if (n < packet.size())
        receive_remainder_.assign(packet.begin() + static_cast<std::ptrdiff_t>(n), packet.end());
    return n;
}

void TcpTransport::disconnect()
{
    net_.stop();
}

bool TcpTransport::isConnected() const
{
    return net_.connected();
}

}  // namespace haven
