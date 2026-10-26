# saurcom_mesh

The packet layers of the LoRa mesh modes, receive side, no keys:

* `pb`: the Protocol Buffers reader of the Meshtastic messages (varint, fixed64, length-delimited, fixed32; groups and reserved wire types end the message), repeated field helpers.
* `meshtastic`: the 16 byte radio header, the `Data` protobuf (portnum first, so random bytes from a wrong key are not taken for a message) and the port payloads Text, Position, NodeInfo,
  Telemetry (device, environment, local stats), Routing, Traceroute and NeighborInfo as a detail line plus a node update or a text message. `names` (generated from meshtastic/protobufs:
  hardware models, ports, roles, routing errors) and `tables` (channel hash, key expansion).
* `meshcore`: header byte, transport codes, path length byte with hash size, path, and the key-free payloads: ADVERT (public key, time, app data with position / name), ACK, TRACE, CONTROL,
  ANON_REQ, the headers of the end-to-end types, and the plain text of a decrypted GRP_TXT / GRP_DATA.
* `lorawan` / `lorawan_names`: MHDR, FHDR (DevAddr, FCtrl, FCnt, FOpts with the MAC command names and the DevStatus / LinkCheck / LinkADR values), FPort, FRMPayload length, MIC, join request
  and accept, rejoin request, the network of a DevAddr (NetID types 0 to 7), device makers by OUI. Payloads are never decrypted.
* `aprs`: LoRa APRS TNC2 splitting and call checks, MeshCom frames (ack, text, position, HEY; 16 bit checksum), the printable / UTF-8 clean text helpers.

Results are flat `repr(C)` records (`abi`): text is length plus bytes in fixed arrays sized for a 255 byte LoRa payload (longer input is cut, never overflowed); numbers "not in this packet" keep the C++
defaults (-1). `unsafe` only in `ffi.rs`, which also holds `cfn`: the C library `snprintf("%.Nf")` that formats the numbers in the detail lines, so the digits equal the C++ reference.

Stays in C++ (`core/src/mesh/`): AES-CTR (Meshtastic), AES-ECB / HMAC-SHA256 / Ed25519 (MeshCore) and the channel tables with their lock (they need keys; the Rust side only parses what they hand
over), the LoRa PHY (`mesh_lora_*`), receiver, generator and report, the packet builders, the region / preset / LoRa settings tables (`meshtasticSlotHz`, `meshcoreDefaults`, `lorawanPresets`, ...), and the
APRS information field parser (`aprs::parse`, itself Rust through saurcom_ax25). The C++ parsing code is deleted from the C++ files (they keep the key-dependent parts, builders and settings tables); `mesh_proto_rs.cpp`, `mesh_proto_lora_rs.cpp` and
`mesh_pb_rs.cpp` are the Rust-backed implementations (`mesh_rs_util.h`: record to std::string conversions, layout check at start-up).

C ABI: `src/ffi.rs`, header `core/include/dect2/rs/mesh.h`, symbols prefixed `saurcom_mesh_`. Golden-vector test: `tests/mesh/test_mesh_proto_rs.cpp` + `rs_part2_mesh.cpp` (digest recorded from the C++ the Rust code replaced): 100000 inputs per family by default, 200000 verified (protobuf, Meshtastic packets of every port with default and user keys, MeshCore adverts / group / direct / TRACE /
random, LoRaWAN, LoRa APRS, MeshCom) plus the name helpers: 5.2 M results hashed at 200000; fuzz target `fuzz_mesh_proto`. Status and conventions: ../README.md.
