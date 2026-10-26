# zorcom_net

The pure protocol layers of the network code, over byte buffers: no sockets, no threads, no allocation (callers pass (pointer, length) buffers; results are ranges of the input or of
a caller-owned arena). Sockets, connections, locks and the radio drivers stay in C++ (`core/src/net/remote_api.cpp`, `core/src/source/source_{rtltcp,spyserver,sdrpp}.cpp`,
`core/src/net/netradio.cpp`); they call the functions of `core/include/dect2/net/net_proto.h`, which is implemented by this crate through `core/src/net/net_proto_rs.cpp`.

* `hash.rs`: SHA-1 (RFC 3174) and base64 (RFC 4648), only for the WebSocket handshake (`Sec-WebSocket-Accept`, RFC 6455 section 4.2.2): the key is no secret, so neither
  needs to be constant time or collision resistant.
* `ws.rs`: server frames (unmasked) and the first client frame of the received bytes (masked, one frame at most 4096 bytes, no fragmentation, text and control opcodes only);
  the answer is "need more", "close with code 1002 / 1003 / 1009" or a frame with its unmasked payload.
* `url.rs`: percent decoding, query parameters, the host part of a Host header, loopback names, the constant time token compare, reason phrases.
* `flatjson.rs`: the request bodies of the remote API, a flat JSON object (at most 32 keys, no nesting), strings unescaped into the caller's arena.
* `http.rs`: where a request head ends (8192 bytes at most), the request line, header fields (at most 48), Content-Length (at most 4096), chunked bodies refused, a POST needs a length.
* `rtltcp.rs`: the 12 byte rtl_tcp greeting, the 5 byte commands, the gain tables and names of the tuners, `host:port` splitting (the port read like the C library's `atoi`).
* `spy.rs`: SpyServer 2.0.1700: DEVICE_INFO and CLIENT_SYNC words, commands, HELLO, SET_SETTING, the 20 byte message header, the rates a server offers.
* `sdrpp.rs`: the SDR++ server protocol: packet headers and command packets, the UI_ACTION argument, and the menu draw list (SmGui) turned into widgets, with the labels made from
  ids and the word flags the rate / gain heuristics of the C++ side use. A rate text ("2.5MHz") is read by the C library's `strtod` (declared in `ffi.rs`) so hexadecimal, `inf`
  and locale quirks come out exactly as in C++.
* `radios.rs`: the list of network radios as the settings keep it (`rtltcp,host,port;pluto,host,0;...`), the host clean-up and the per-type tables.

What deliberately stays in C++: the sockets and threads, the IQ sample conversion of the three clients (hot loops, no parsing), the gain plan and rate combo heuristics of the SDR++
menu (they work on the widgets), the Hermes Lite 2 / Red Pitaya discovery packets, SoapyRemote glue, and `share_server.cpp` (a socket loop).

Licences: all of this is a translation of Eye of ZorCom's own C++ (SHA-1 and base64 were written for Eye of ZorCom from RFC 3174 / RFC 4648); the SpyServer and SDR++ message formats are
implemented from the public protocol descriptions, no code of those projects is copied, so no third party notice is needed. Anything ported from elsewhere in future must keep its
notice here and in the THIRD_PARTY notes.

Verification: `tests/net/test_net_proto_rs.cpp` (golden-vector test: a digest of 100000 inputs per family, recorded from the C++ it replaced, ZORCOM_DIFF_ITERS to change; plus the C ABI with null pointers and short arrays), `fuzz_net_proto` (seeded mutations for the sanitizer build, also a libFuzzer target), Rust unit tests with known vectors.
Status and conventions: ../README.md.
