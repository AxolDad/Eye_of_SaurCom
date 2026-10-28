// A small WebSocket client (RFC 6455) over the TCP helpers of tcp_client.h, for the SDRconnect API of the nRSP-ST (source_nrspst.cpp):
// the upgrade handshake (ws://, no TLS), masked text and binary frames out, frames in with fragmentation, ping/pong, close, and a limit
// on the size of one message. A blocking client: reads give up after the socket's receive timeout (sockRecvTimeout).
#pragma once
#include "tcp_client.h"
#include "dect2/net/remote_api.h"
#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace dect2 {

class WsClient {
public:
    enum class Kind { Text, Binary, Closed, Timeout, Bad };   // what readMessage() brought
    static constexpr size_t kDefaultMaxMessage = 4u << 20;

    WsClient() = default;
    void attach(sock_t s) { s_ = s; lastFrame_ = std::chrono::steady_clock::now(); }
    sock_t sock() const { return s_; }
    // When the last frame of any kind (pings, pongs and close included) began to arrive; attach() and a good handshake count as one. For a
    // keepalive: a peer that answers nothing, not even a ping, is gone.
    std::chrono::steady_clock::time_point lastFrameAt() const { return lastFrame_; }

    // The upgrade request and its answer: 101 with the matching Sec-WebSocket-Accept. err says why not, in plain words.
    bool handshake(const std::string& host, int port, const std::string& path, std::string& err) {
        unsigned char r[16];
        std::random_device rd;
        for (auto& b : r) b = (unsigned char)rd();
        static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string key;   // base64 of 16 bytes: 22 characters and "=="
        for (int i = 0; i < 15; i += 3) {
            const uint32_t v = (uint32_t)r[i] << 16 | (uint32_t)r[i + 1] << 8 | r[i + 2];
            key += a[v >> 18 & 63]; key += a[v >> 12 & 63]; key += a[v >> 6 & 63]; key += a[v & 63];
        }
        key += a[r[15] >> 2]; key += a[(r[15] & 3) << 4]; key += "==";
        const std::string hp = (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
        const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + hp + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                                "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        if (!sockSendAll(s_, req.data(), req.size())) { err = "the connection dropped during the WebSocket handshake"; return false; }
        std::string head;
        char c;
        while (head.size() < 8192) {   // byte by byte: whatever follows the header belongs to the frames
            if (sockRecv(s_, &c, 1) <= 0) { err = head.empty() ? "no answer to the WebSocket handshake" : "the connection dropped during the WebSocket handshake"; return false; }
            head += c;
            if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) break;
        }
        if (head.size() >= 8192 || head.compare(head.size() - 4, 4, "\r\n\r\n") != 0) { err = "not a WebSocket server (the answer is not an HTTP header)"; return false; }
        if (head.compare(0, 5, "HTTP/") != 0 || head.find(" 101") == std::string::npos || head.find(" 101") > 12) {
            const size_t e = head.find("\r\n");
            err = "not a WebSocket server, or it refused: " + head.substr(0, std::min<size_t>(e, 60));
            return false;
        }
        std::string low = head;
        for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
        const size_t p = low.find("sec-websocket-accept:");
        if (p == std::string::npos) { err = "the server's WebSocket handshake has no Sec-WebSocket-Accept"; return false; }
        size_t b = p + 21, e = head.find("\r\n", b);
        while (b < e && head[b] == ' ') b++;
        if (head.substr(b, e - b) != wsAcceptKey(key)) { err = "the server's WebSocket handshake answer does not match (not a WebSocket server)"; return false; }
        lastFrame_ = std::chrono::steady_clock::now();
        return true;
    }

    bool sendText(const std::string& s) { return sendFrame(0x1, s.data(), s.size()); }
    bool sendBinary(const void* d, size_t n) { return sendFrame(0x2, d, n); }
    void sendClose() { sendFrame(0x8, "\x03\xe8", 2); }
    bool sendPing() { return sendFrame(0x9, "", 0); }   // the answer (a pong) is dropped by readMessage() but moves lastFrameAt()

    // One whole message (fragments joined; pings answered, pongs dropped). Closed: a close frame or the connection went. Timeout: nothing
    // came within the receive timeout. Bad: not a valid frame, a message over maxMessage, or a masked frame from the server.
    Kind readMessage(std::vector<unsigned char>& out, size_t maxMessage = kDefaultMaxMessage) {
        out.clear();
        int kind = -1;   // opcode of the first fragment
        for (;;) {
            unsigned char h[2];
            const int r0 = recvExact(h, 2);
            if (r0 != 1) return r0 == 0 ? Kind::Timeout : Kind::Closed;
            lastFrame_ = std::chrono::steady_clock::now();
            const bool fin = h[0] & 0x80;
            const int op = h[0] & 0x0F;
            if (h[0] & 0x70) return Kind::Bad;   // no extensions were negotiated
            if (h[1] & 0x80) return Kind::Bad;   // servers do not mask
            uint64_t len = h[1] & 0x7F;
            if (len == 126) { unsigned char x[2]; if (recvExact(x, 2) != 1) return Kind::Closed; len = (uint64_t)x[0] << 8 | x[1]; }
            else if (len == 127) {
                unsigned char x[8];
                if (recvExact(x, 8) != 1) return Kind::Closed;
                len = 0;
                for (int i = 0; i < 8; i++) len = len << 8 | x[i];
            }
            if (op >= 0x8) {   // control frames: short, never fragmented
                if (len > 125 || !fin) return Kind::Bad;
                std::vector<unsigned char> p((size_t)len);
                if (len && recvExact(p.data(), p.size()) != 1) return Kind::Closed;
                if (op == 0x8) { sendFrame(0x8, p.data(), p.size() >= 2 ? 2 : 0); return Kind::Closed; }
                if (op == 0x9) sendFrame(0xA, p.data(), p.size());
                else if (op != 0xA) return Kind::Bad;
                continue;
            }
            if (op == 0x0 ? kind < 0 : kind >= 0) return Kind::Bad;   // a continuation with nothing to continue, or a new message in the middle of one
            if (op != 0x0) { if (op != 0x1 && op != 0x2) return Kind::Bad; kind = op; }
            if (len > maxMessage || out.size() + len > maxMessage) return Kind::Bad;
            const size_t at = out.size();
            out.resize(at + (size_t)len);
            if (len && recvExact(out.data() + at, (size_t)len) != 1) return Kind::Closed;
            if (fin) return kind == 0x1 ? Kind::Text : Kind::Binary;
        }
    }

private:
    // 1: all n bytes. 0: nothing came within the timeout (at the start of a frame only). -1: the connection went, or stopped in a frame.
    int recvExact(void* d, size_t n) {
        char* p = (char*)d;
        size_t got = 0;
        while (got < n) {
            const long long k = sockRecv(s_, p + got, n - got);
            if (k <= 0) return got == 0 && k < 0 && timedOut() ? 0 : -1;
            got += (size_t)k;
        }
        return 1;
    }
    static bool timedOut() {
#ifdef _WIN32
        const int e = WSAGetLastError();
        return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
    }
    bool sendFrame(int op, const void* d, size_t n) {
        std::vector<unsigned char> f;
        f.push_back((unsigned char)(0x80 | op));
        if (n < 126) f.push_back((unsigned char)(0x80 | n));
        else if (n < 65536) { f.push_back(0x80 | 126); f.push_back((unsigned char)(n >> 8)); f.push_back((unsigned char)n); }
        else { f.push_back(0x80 | 127); for (int i = 7; i >= 0; i--) f.push_back((unsigned char)((uint64_t)n >> (8 * i))); }
        std::random_device rd;
        unsigned char m[4];
        for (auto& b : m) { b = (unsigned char)rd(); f.push_back(b); }
        const unsigned char* p = (const unsigned char*)d;
        for (size_t i = 0; i < n; i++) f.push_back(p[i] ^ m[i & 3]);
        std::lock_guard<std::mutex> lk(sendMu_);
        return sockSendAll(s_, f.data(), f.size());
    }

    sock_t s_ = kBadSock;
    std::mutex sendMu_;
    std::chrono::steady_clock::time_point lastFrame_ = std::chrono::steady_clock::now();
};

} // namespace dect2
