#pragma once
// Thin, safe wrappers around POSIX TCP sockets, plus length-prefixed framing.
//
// WHY FRAMING?  TCP is a *byte stream*, not a message stream. If the agent
// sends two 500-byte messages, the server may receive 1000 bytes in one
// recv(), or 3 bytes, then 997... There are no message boundaries.
// So every message is sent as:
//
//     [ 4-byte length, big-endian ][ payload of exactly that many bytes ]
//
// The reader first reads exactly 4 bytes, then exactly `length` bytes.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace sysmon::net {

struct NetError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Largest payload we accept. Protects the server from a buggy or malicious
// client claiming a 4 GB message and making us allocate it.
constexpr std::uint32_t kMaxFrameBytes = 1u << 20;  // 1 MiB

// RAII owner of a file descriptor: closes it automatically, can be moved
// but not copied (two owners would close the same fd twice).
class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket() { reset(); }

    Socket(Socket&& other) noexcept : fd_(other.release()) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    int fd() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }
    int release() {
        int f = fd_;
        fd_ = -1;
        return f;
    }
    void reset(int new_fd = -1);

private:
    int fd_ = -1;
};

enum class ReadStatus {
    Ok,       // got all requested bytes
    Closed,   // peer closed the connection cleanly
    Stopped,  // our stop flag was set while waiting
    Error,    // socket error, or a frame that breaks the protocol
};

// Sends all bytes, looping over partial writes. Returns false on failure.
// Uses MSG_NOSIGNAL: writing to a closed socket would otherwise raise SIGPIPE,
// whose default action KILLS the process.
bool write_all(int fd, const void* data, std::size_t len);

// Reads exactly `len` bytes, looping over partial reads. Waits with poll() in
// short slices so it can notice `stop` being set (if given).
ReadStatus read_exact(int fd, void* data, std::size_t len,
                      const std::atomic<bool>* stop = nullptr, int poll_ms = 200);

bool send_frame(int fd, const std::string& payload);
ReadStatus recv_frame(int fd, std::string& payload, const std::atomic<bool>* stop = nullptr);

// Client side: resolves `host` and connects, giving up after timeout_ms.
// Throws NetError on failure.
Socket connect_tcp(const std::string& host, std::uint16_t port, int timeout_ms = 3000);

// Server side: listening socket on all interfaces. port 0 = let the OS pick
// a free port (used by tests). Throws NetError on failure.
Socket listen_tcp(std::uint16_t port, int backlog = 64);

// The port a socket is bound to (useful after listen_tcp(0)).
std::uint16_t local_port(int fd);

// True if the peer has closed the connection (we received its FIN) or the
// socket is in an error state. Non-blocking: only peeks, never consumes data.
// Used by the client before each send: our server never sends data to the
// agent, so "readable" on the agent's socket can only mean "closed".
bool peer_closed(int fd);

// Applies a send timeout so a write to a stuck peer cannot block forever.
void set_send_timeout(int fd, int timeout_ms);

}  // namespace sysmon::net
