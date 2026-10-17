#pragma once

#include "net/ITransport.h"
#include "net_client.h"

#include <cstdint>
#include <string>
#include <vector>

namespace haven {

/// A TCP link to an already-running server.
///
/// The connect is non-blocking with a timeout, because a blocking connect to
/// an unreachable host stalls for the operating system's timeout (about 20 s
/// on Windows) — far longer than the main menu should ever freeze. Once the
/// link is up, the socket is handed to NetClient, whose dedicated thread owns
/// every later read, write and close, so the render loop never blocks on the
/// network.
class TcpTransport : public ITransport {
public:
    TcpTransport() = default;
    ~TcpTransport() override;

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    bool connect(const std::string& host, uint16_t port, std::string& outError) override;
    void update(float dtSeconds) override;
    bool sendPacket(const uint8_t* data, size_t len) override;
    size_t receiveBytes(uint8_t* outBuf, size_t maxLen) override;
    void disconnect() override;
    bool isConnected() const override;

private:
    NetClient net_;

    /// The tail of a packet larger than the caller's last buffer. NetClient
    /// hands over whole framed packets, so a read that would have split one
    /// keeps the remainder here rather than losing or reordering bytes.
    std::vector<uint8_t> receive_remainder_;
};

}  // namespace haven
