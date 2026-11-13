#include "sysmon/socket.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace sysmon::net {

void Socket::reset(int new_fd) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = new_fd;
}

bool write_all(int fd, const void* data, std::size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;  // interrupted by a signal: just retry
            return false;                  // EPIPE, ECONNRESET, EAGAIN (send timeout)...
        }
        // send() may write FEWER bytes than asked (partial write): keep going.
        p += n;
        len -= static_cast<std::size_t>(n);
    }
    return true;
}

ReadStatus read_exact(int fd, void* data, std::size_t len, const std::atomic<bool>* stop,
                      int poll_ms) {
    char* p = static_cast<char*>(data);
    while (len > 0) {
        // Wait until data is available, in short slices so we can check `stop`.
        pollfd pfd{fd, POLLIN, 0};
        int r = ::poll(&pfd, 1, poll_ms);
        if (r < 0) {
            if (errno == EINTR) continue;
            return ReadStatus::Error;
        }
        if (r == 0) {  // timeout slice elapsed, nothing to read yet
            if (stop && stop->load()) return ReadStatus::Stopped;
            continue;
        }

        ssize_t n = ::recv(fd, p, len, 0);
        if (n == 0) return ReadStatus::Closed;  // peer closed the connection
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return ReadStatus::Error;
        }
        // recv() may return FEWER bytes than asked (partial read): keep going.
        p += n;
        len -= static_cast<std::size_t>(n);
    }
    return ReadStatus::Ok;
}

bool send_frame(int fd, const std::string& payload) {
    if (payload.size() > kMaxFrameBytes) return false;
    // Network byte order (big-endian), so machines with different CPUs agree.
    std::uint32_t len_be = htonl(static_cast<std::uint32_t>(payload.size()));

    // Header and payload in ONE buffer -> one send() call in the common case,
    // instead of a tiny 4-byte packet followed by the payload.
    std::string buf(sizeof(len_be) + payload.size(), '\0');
    std::memcpy(buf.data(), &len_be, sizeof(len_be));
    std::memcpy(buf.data() + sizeof(len_be), payload.data(), payload.size());
    return write_all(fd, buf.data(), buf.size());
}

ReadStatus recv_frame(int fd, std::string& payload, const std::atomic<bool>* stop) {
    std::uint32_t len_be = 0;
    ReadStatus st = read_exact(fd, &len_be, sizeof(len_be), stop);
    if (st != ReadStatus::Ok) return st;

    std::uint32_t len = ntohl(len_be);
    if (len > kMaxFrameBytes) return ReadStatus::Error;  // refuse absurd sizes

    payload.assign(len, '\0');
    if (len == 0) return ReadStatus::Ok;
    st = read_exact(fd, payload.data(), len, stop);
    // Peer closing in the MIDDLE of a frame is a protocol error, not a clean close.
    return st == ReadStatus::Closed ? ReadStatus::Error : st;
}

bool peer_closed(int fd) {
    pollfd pfd{fd, POLLIN | POLLRDHUP, 0};
    int r = ::poll(&pfd, 1, 0);  // timeout 0: just check, don't wait
    if (r <= 0) return false;    // nothing pending: connection looks alive
    if (pfd.revents & (POLLERR | POLLHUP | POLLRDHUP | POLLNVAL)) return true;
    if (pfd.revents & POLLIN) {
        char c;
        // MSG_PEEK looks at the data without removing it. recv() == 0 means FIN.
        ssize_t n = ::recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
        return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
    }
    return false;
}

void set_send_timeout(int fd, int timeout_ms) {
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

Socket connect_tcp(const std::string& host, std::uint16_t port, int timeout_ms) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;  // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port_str = std::to_string(port);
    int rc = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (rc != 0) throw NetError("resolve " + host + ": " + ::gai_strerror(rc));

    std::string last_error = "no addresses";
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        Socket s(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (!s) continue;
        // On Linux, SO_SNDTIMEO also bounds how long connect() may block.
        set_send_timeout(s.fd(), timeout_ms);
        if (::connect(s.fd(), ai->ai_addr, ai->ai_addrlen) == 0) {
            int one = 1;
            // Small messages: send immediately rather than batching (Nagle's algorithm).
            ::setsockopt(s.fd(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            ::freeaddrinfo(res);
            return s;
        }
        // With SO_SNDTIMEO, a connect() that gets no answer in time fails with
        // EINPROGRESS ("Operation now in progress"). Say what it really means.
        last_error = (errno == EINPROGRESS || errno == EAGAIN)
                         ? "timed out (no reply - is the server running?)"
                         : std::strerror(errno);
    }
    ::freeaddrinfo(res);
    throw NetError("connect " + host + ":" + port_str + ": " + last_error);
}

Socket listen_tcp(std::uint16_t port, int backlog) {
    Socket s(::socket(AF_INET, SOCK_STREAM, 0));
    if (!s) throw NetError(std::string("socket: ") + std::strerror(errno));

    // Lets the server restart immediately on the same port. Without it, the
    // port stays blocked for ~60 s in TIME_WAIT after the previous run.
    int one = 1;
    ::setsockopt(s.fd(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  // all network interfaces
    addr.sin_port = htons(port);
    if (::bind(s.fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        throw NetError("bind port " + std::to_string(port) + ": " + std::strerror(errno));
    }
    if (::listen(s.fd(), backlog) != 0) {
        throw NetError(std::string("listen: ") + std::strerror(errno));
    }
    return s;
}

std::uint16_t local_port(int fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

}  // namespace sysmon::net
