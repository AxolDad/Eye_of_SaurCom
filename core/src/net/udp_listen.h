// One UDP socket and its receive thread, shared by the CoT and WiFi sensing listeners (cot_rx, wifisense_rx).
#pragma once
#include "dect2/common/thread_name.h"
#include "netcompat.h"
#include "tcp_client.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace dect2 {

struct UdpListen {
    using Handler = std::function<void(const uint8_t*, size_t, const std::string& fromIp)>;

    // Binds port on every interface; joins group when it is a multicast address (224.0.0.0/4). reuse: share the port with others.
    bool open(const std::string& group, int port, bool reuse, std::string& err) {
        netInit();
        if (port <= 0 || port > 65535) { err = "port out of range"; return false; }
        sock_t sk = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sk == kBadSock) { err = "no UDP socket"; return false; }
        if (reuse) { const int one = 1; setsockopt(sk, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one); }
        sockaddr_in a{};
        a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(sk, (const sockaddr*)&a, sizeof a) != 0) { sockClose(sk); err = "cannot listen on UDP port " + std::to_string(port); return false; }
        in_addr g{};
        if (!group.empty() && inet_pton(AF_INET, group.c_str(), &g) == 1 && (ntohl(g.s_addr) >> 28) == 0xE) {   // 224.0.0.0/4: join it
            ip_mreq mr{};
            mr.imr_multiaddr = g; mr.imr_interface.s_addr = htonl(INADDR_ANY);
            if (setsockopt(sk, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mr, sizeof mr) != 0) { sockClose(sk); err = "cannot join " + group; return false; }
        }
        sockRecvTimeout(sk, 300);   // so stop() is noticed
        s_ = sk;
        return true;
    }

    // Calls h for each datagram (up to bufSize bytes) on a thread named name, until stop().
    void run(const char* name, size_t bufSize, Handler h) {
        quit_ = false;
        th_ = std::thread([this, name, bufSize, h = std::move(h)] {
            setThreadName(name);
            std::vector<uint8_t> buf(bufSize);
            while (!quit_) {
                sockaddr_in src{};
                socklen_t sl = sizeof src;
                const long long n = recvfrom(s_, (char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&src, &sl);
                if (n == 0) continue;   // an empty datagram
                if (n < 0) {
                    if (!timedOut()) std::this_thread::sleep_for(std::chrono::milliseconds(50));   // a persistent error must not spin
                    continue;
                }
                char ip[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &src.sin_addr, ip, sizeof ip);
                h(buf.data(), (size_t)n, ip);
            }
        });
    }

    ~UdpListen() { stop(); }
    void stop() {
        quit_ = true;
        if (th_.joinable()) th_.join();
        if (s_ != kBadSock) { sockClose(s_); s_ = kBadSock; }
    }

private:
    // the last recv gave up on the receive timeout (or was interrupted)
    static bool timedOut() {
#ifdef _WIN32
        const int e = WSAGetLastError();
        return e == WSAETIMEDOUT || e == WSAEINTR || e == WSAEMSGSIZE;   // a datagram larger than the buffer is dropped, not an outage
#else
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
    }

    sock_t s_ = kBadSock;
    std::thread th_;
    std::atomic<bool> quit_{false};
};

} // namespace dect2
