#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace haven {

/// The byte-pump half of the client's network layer, in the shape GameSession
/// needs it. Everything above this interface speaks packets; everything below
/// it speaks sockets, subprocesses or — later — replay files.
///
/// A transport is synchronous to connect and asynchronous to run: connect()
/// blocks until the link is up (or has definitely failed), after which
/// update() pumps whatever background I/O the implementation has and
/// receiveBytes() hands over what arrived.
///
/// Implementations must be safe to disconnect() more than once, and safe to
/// disconnect() after a failed connect().
class ITransport {
public:
    virtual ~ITransport() = default;

    /// Opens the link. On failure returns false and fills `outError` with a
    /// message fit to show the player. On success the transport is connected
    /// and isConnected() is true.
    virtual bool connect(const std::string& host, uint16_t port, std::string& outError) = 0;

    /// Pumps background I/O. Called once per rendered frame.
    virtual void update(float dtSeconds) = 0;

    /// Queues one packet for delivery. Returns false when the link is down or
    /// the packet was refused, so the caller learns it never went out.
    virtual bool sendPacket(const uint8_t* data, size_t len) = 0;

    /// Hands over the next `maxLen` bytes received since the last call, or 0
    /// when nothing has arrived. A short read is not an error: the remainder
    /// of a packet larger than `maxLen` stays queued for the next call.
    virtual size_t receiveBytes(uint8_t* outBuf, size_t maxLen) = 0;

    /// Tears the link down and releases every resource the transport owns.
    virtual void disconnect() = 0;

    /// True between a successful connect() and a disconnect().
    virtual bool isConnected() const = 0;
};

}  // namespace haven
