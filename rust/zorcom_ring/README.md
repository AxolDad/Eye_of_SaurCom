# zorcom_ring

Rust implementation of `IqRing` (`core/include/dect2/common/ring.h`): lock-free SPSC ring of complex float samples with the same API and
accounting (overrun drops counted, `skipToNewest`, `dropBacklog`, `clear`, `noteLost`, seqlock `lastWrite`). Storage and the header block are owned by
the C++ wrapper (`IqRingRs` in ring.h); the crate is `no_std`, no dependencies. Memory orderings are justified in `src/ring.rs`.

Opt-in: `-DDECT2_RUST_RING=ON` makes `IqRing` the Rust one (default OFF). Not ported: `BoundedQueue<T>`, `ReorderBuffer<T>`, `ThreadPool`,
`StagePipeline<Item>` are templates over arbitrary C++ types with mutex/condvar semantics; a Rust MPMC queue would not have the same
contract across the C ABI, so they stay in C++.

Tests: `cargo test -p zorcom_ring` (model comparison, wrapped counters, threaded stress, exhaustive interleavings of a step model);
`test_ring_rs` (ctest `common_ring_rs`): C++ ring vs Rust ring (both stay, the Rust one is opt-in), two-thread stress; `test_ring_rs --bench`: ns/op.
