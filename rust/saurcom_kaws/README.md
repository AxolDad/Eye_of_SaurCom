# saurcom_kaws

KAWS, the owner's HF weak-signal mode (2.4 kHz, UTC slotted; chat, beacon, very low rate voice). Spec: `KAWS-SPEC-v0.1`. Signals are complex baseband at 2400 samples/s
(one sample per chip, audio frequency = 1500 Hz + baseband frequency). `no_std` + `alloc`, no external crates.

| module | content |
|--------|---------|
| `profile` | the ladder P0..P7 (chirp SF10..SF6 and OFDM) |
| `chirp` | cyclic-shift chirp symbols, Gray mapping, the weave LFSR, bin log-likelihoods |
| `ldpc` | systematic quasi-cyclic IRA codes for n = 512 .. 1728 (the Kris construction at short lengths), layered min-sum |
| `link` | interleaver, burst assembly (lattice units, midambles, closing weave), the chirp receiver (iterative demapping, multipath lag weights, EM), the OFDM receiver |
| `lattice` | the woven-lattice preamble unit, closing weave, detector (rising / falling pair statistic), path weights, closing-weave offset estimate |
| `ofdm` | 48 carriers at 50 Hz, pilots every 4th carrier, delay-domain channel estimation with decision-directed refinement |
| `watterson` | KW-AWGN / KW-MOD / KW-POOR channel with offset, drift, clock error, interferer |
| `audio` | 2400 samples/s baseband to 12 kHz real audio and back |
| `ffi` | the C ABI (`core/include/dect2/rs/kaws.h`) |

Tests: `cargo test --release -p saurcom_kaws` (fast unit and smoke tests). The measurement harness (`tests/thresholds.rs`, all `#[ignore]`) produces the tables in
`KAWS-MEASURED-v0.1.md`: `thr_chirp` (payload only, genie sync), `thr_sweep_full` (the whole chain), `noise_only_detect` (false alarm statistics), `probe`,
`conv_baseline.rs` (K = 7 convolutional code against the LDPC code for P0).
