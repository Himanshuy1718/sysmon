// Framing tests over a socketpair: two connected sockets inside one process,
// so we control exactly how bytes arrive.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "sysmon/socket.hpp"

using namespace std::chrono_literals;
using namespace sysmon::net;

namespace {
// Raw write for building hand-made byte streams in tests.
void raw_write(int fd, const void* data, size_t len) {
    ASSERT_EQ(::write(fd, data, len), static_cast<ssize_t>(len));
}

struct Pair {
    Socket a, b;
    Pair() {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) throw std::runtime_error("socketpair");
        a.reset(fds[0]);
        b.reset(fds[1]);
    }
};
}  // namespace

TEST(Framing, SendAndReceiveOneFrame) {
    Pair p;
    ASSERT_TRUE(send_frame(p.a.fd(), "hello world"));
    std::string got;
    ASSERT_EQ(recv_frame(p.b.fd(), got), ReadStatus::Ok);
    EXPECT_EQ(got, "hello world");
}

TEST(Framing, KeepsMessageBoundaries) {
    Pair p;
    // Three frames written back to back arrive as one continuous byte stream...
    send_frame(p.a.fd(), "first");
    send_frame(p.a.fd(), "");
    send_frame(p.a.fd(), "third message");
    // ...but the reader still gets them out one by one.
    std::string got;
    recv_frame(p.b.fd(), got);
    EXPECT_EQ(got, "first");
    recv_frame(p.b.fd(), got);
    EXPECT_EQ(got, "");
    recv_frame(p.b.fd(), got);
    EXPECT_EQ(got, "third message");
}

// The key TCP property: data can arrive in arbitrarily small pieces.
TEST(Framing, ReassemblesFrameSentOneByteAtATime) {
    Pair p;
    const std::string payload = R"({"type":"hello","host":"node-1","version":1})";
    uint32_t len_be = htonl(static_cast<uint32_t>(payload.size()));
    std::string wire(reinterpret_cast<char*>(&len_be), 4);
    wire += payload;

    std::thread writer([&] {
        for (char c : wire) {
            raw_write(p.a.fd(), &c, 1);
            std::this_thread::sleep_for(1ms);
        }
    });
    std::string got;
    EXPECT_EQ(recv_frame(p.b.fd(), got), ReadStatus::Ok);
    writer.join();
    EXPECT_EQ(got, payload);
}

TEST(Framing, RejectsOversizedFrame) {
    Pair p;
    uint32_t huge = htonl(kMaxFrameBytes + 1);  // claims more than 1 MiB
    raw_write(p.a.fd(), &huge, 4);
    std::string got;
    EXPECT_EQ(recv_frame(p.b.fd(), got), ReadStatus::Error);
}

TEST(Framing, CleanCloseVersusCloseMidFrame) {
    {
        Pair p;
        p.a.reset();  // close before sending anything
        std::string got;
        EXPECT_EQ(recv_frame(p.b.fd(), got), ReadStatus::Closed);
    }
    {
        Pair p;
        uint32_t len_be = htonl(100);
        raw_write(p.a.fd(), &len_be, 4);
        raw_write(p.a.fd(), "only ten b", 10);  // promised 100 bytes, sent 10
        p.a.reset();
        std::string got;
        EXPECT_EQ(recv_frame(p.b.fd(), got), ReadStatus::Error);
    }
}

TEST(Framing, StopFlagInterruptsWaitingReader) {
    Pair p;
    std::atomic<bool> stop{false};
    std::thread stopper([&] {
        std::this_thread::sleep_for(100ms);
        stop = true;
    });
    std::string got;
    auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(recv_frame(p.b.fd(), got, &stop), ReadStatus::Stopped);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
    stopper.join();
}

TEST(Framing, WriteToClosedPeerFailsInsteadOfKillingProcess) {
    Pair p;
    p.b.reset();
    // Without MSG_NOSIGNAL this would raise SIGPIPE and terminate the test binary.
    bool ok = true;
    for (int i = 0; i < 10 && ok; ++i) ok = send_frame(p.a.fd(), "data");
    EXPECT_FALSE(ok);
}

TEST(Framing, PeerClosedDetectsFin) {
    Pair p;
    EXPECT_FALSE(peer_closed(p.a.fd()));  // open, nothing pending
    p.b.reset();                          // other side closes -> FIN
    EXPECT_TRUE(peer_closed(p.a.fd()));
}

TEST(Framing, PeerClosedIgnoresPendingData) {
    Pair p;
    send_frame(p.b.fd(), "data");          // readable because of DATA, not a close
    EXPECT_FALSE(peer_closed(p.a.fd()));
    std::string got;
    EXPECT_EQ(recv_frame(p.a.fd(), got), ReadStatus::Ok);  // data was not consumed
    EXPECT_EQ(got, "data");
}

TEST(Tcp, ConnectToClosedPortThrows) {
    Socket listener = listen_tcp(0);
    uint16_t port = local_port(listener.fd());
    listener.reset();  // nobody listening there any more
    EXPECT_THROW(connect_tcp("127.0.0.1", port, 1000), NetError);
}
