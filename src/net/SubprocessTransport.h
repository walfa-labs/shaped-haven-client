#pragma once

#include "net/ITransport.h"
#include "net/TcpTransport.h"

#include <cstdint>
#include <string>

namespace haven {

/// Single-player link: the server executable as a child process.
///
/// connect() spawns the server, waits for it to start listening, then hands
/// the link to a TcpTransport exactly as if the player had chosen multiplayer.
/// disconnect() terminates the child, so leaving a run never leaves an orphan
/// server holding the port.
class SubprocessTransport : public ITransport {
public:
    explicit SubprocessTransport(std::string server_path);
    ~SubprocessTransport() override;

    SubprocessTransport(const SubprocessTransport&) = delete;
    SubprocessTransport& operator=(const SubprocessTransport&) = delete;

    bool connect(const std::string& host, uint16_t port, std::string& outError) override;
    void update(float dtSeconds) override;
    bool sendPacket(const uint8_t* data, size_t len) override;
    size_t receiveBytes(uint8_t* outBuf, size_t maxLen) override;
    void disconnect() override;
    bool isConnected() const override;

private:
    bool spawn_server(uint16_t port, std::string& outError);
    bool wait_for_port(uint16_t port, std::string& outError);
    bool port_open(uint16_t port) const;
    void terminate_server();

    std::string server_path_;
    TcpTransport tcp_;
    void* process_ = nullptr;  // HANDLE; windows.h is not included here
};

}  // namespace haven
