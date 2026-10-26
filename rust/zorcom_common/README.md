# zorcom_common

Shared, dependency-free, allocation-free building blocks used by every protocol crate:
generic CRC (width 1..=32, any poly/init/reflection/xorout) with CCITT-FALSE, X.25 and CRC-32 presets,
packed and one-bit-per-byte bit access (MSB or LSB first), endian helpers, hex, parity, Gray code,
NRZI, Manchester and a small fixed-size ring. No `unsafe`, no C ABI: protocol crates call it from safe code.
