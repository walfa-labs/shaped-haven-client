#include "net/SubprocessTransport.h"

// winsock2.h must come before windows.h: it defines _WINSOCK2API_, which is
// what stops windows.h from pulling in the conflicting winsock.h. windows.h
// also defines min/max as macros.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace haven {

/// How often to probe the port while waiting for the server to come up.
constexpr int kServerReadyPollMs = 100;
/// How long to wait for the server to start listening before giving up.
constexpr int kServerReadyTimeoutMs = 5000;
/// How long to wait for a terminated server to actually exit. It has to be gone
/// before a restart can bind the port, so this is a wait rather than a leak.
constexpr int kTerminateTimeoutMs = 5000;

namespace {

/// The server executable path is carried as UTF-8 so a non-ASCII install
/// directory survives the trip from the command line.
std::wstring to_wide(const std::string& text)
{
    if (text.empty()) return {};
    const int len = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), len);
    return out;
}

}  // namespace

SubprocessTransport::SubprocessTransport(std::string server_path) : server_path_(std::move(server_path)) {}

SubprocessTransport::~SubprocessTransport()
{
    disconnect();
}

bool SubprocessTransport::connect(const std::string& host, uint16_t port, std::string& outError)
{
    if (!spawn_server(port, outError)) return false;

    // The server is a fresh process: it has to start the Zig runtime and bind
    // its listener before anything can connect. Poll rather than sleep a
    // fixed amount, so a fast start is not slowed down and a slow one is not
    // given up on early.
    if (!wait_for_port(port, outError))
    {
        terminate_server();
        return false;
    }

    if (!tcp_.connect(host, port, outError))
    {
        terminate_server();
        return false;
    }
    return true;
}

void SubprocessTransport::update(float dtSeconds)
{
    tcp_.update(dtSeconds);
}

bool SubprocessTransport::sendPacket(const uint8_t* data, size_t len)
{
    return tcp_.sendPacket(data, len);
}

size_t SubprocessTransport::receiveBytes(uint8_t* outBuf, size_t maxLen)
{
    return tcp_.receiveBytes(outBuf, maxLen);
}

void SubprocessTransport::disconnect()
{
    tcp_.disconnect();
    terminate_server();
}

bool SubprocessTransport::isConnected() const
{
    return tcp_.isConnected();
}

bool SubprocessTransport::spawn_server(uint16_t port, std::string& outError)
{
    // CreateProcess may modify the command line, so it gets a mutable buffer.
    // The path is quoted so a directory with spaces survives.
    std::wstring command = L"\"" + to_wide(server_path_) + L"\" --port " + std::to_wstring(static_cast<unsigned>(port));

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    // CREATE_NO_WINDOW keeps the headless server from flashing a console.
    if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                          &startup, &info))
    {
        outError = "could not start '" + server_path_ + "': " + std::to_string(::GetLastError());
        return false;
    }
    ::CloseHandle(info.hThread);
    process_ = info.hProcess;
    return true;
}

bool SubprocessTransport::wait_for_port(uint16_t port, std::string& outError)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kServerReadyTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (port_open(port)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(kServerReadyPollMs));
    }
    outError = "server did not start listening on port " + std::to_string(static_cast<unsigned>(port)) + " within " +
               std::to_string(kServerReadyTimeoutMs) + " ms";
    return false;
}

bool SubprocessTransport::port_open(uint16_t port) const
{
    // A bare connect and immediate close is enough to prove the server is
    // listening. The server treats a peer that closes without sending C2S_HELLO
    // as a dropped connection and unwinds it, so the probe leaves no player
    // slot behind.
    const int sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == -1) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    const bool open = ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::closesocket(sock);
    return open;
}

void SubprocessTransport::terminate_server()
{
    if (process_ == nullptr) return;
    ::TerminateProcess(process_, 0);
    // Wait for the exit rather than leaking the handle: a restart spawns a new
    // server, and it cannot bind the port until this one is gone.
    ::WaitForSingleObject(process_, kTerminateTimeoutMs);
    ::CloseHandle(process_);
    process_ = nullptr;
}

}  // namespace haven
