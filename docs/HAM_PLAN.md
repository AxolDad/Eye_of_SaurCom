# Ham operation plan (goals section 7)

Goal: WSJT-X style operation for every amateur mode, for people with a rig. The switch at the top left is now RX / TX / Ham.

## What exists (inventory, 2026-10-20)

| Part | Where | State |
|---|---|---|
| Hamlib CAT | `core/include/dect2/source/hamlib_rig.h`, `rig_setup.h`, `core/src/source/hamlib_rig.cpp` | Rig list, serial/rigctld, dial + mode follow. `makeHamlibTxSink`: PTT and frequency only, **no RF audio** (the note says so) |
| TX session | `core/include/dect2/tx/tx.h`, `core/src/tx/tx.cpp` | One continuous or timed session per mode on a TX-capable SDR; FT8/FT4/WSPR/FT2/JT65/JT9/Q65/FST4/MSK144 payload kind `Ftx`; no per-slot scheduling |
| Safety today | `app/panels/tx_ui.cpp` (`txNoticePopup`, Start, STOP ALL) | One-time notice, explicit Start, STOP ALL. No band check, no arm. "OnAir does not limit the frequency" |
| Modulators | `core/src/hfdig/hfdig_ftx_gen.cpp` (`ftxAddTransmission[Ex]`), `hfdig_gen`, `hfdig_sstv_gen`, `hfdig_hamdrm_gen` | Audio for FT8/FT4/FT2/WSPR/JT/Q65/FST4/MSK144 messages at any audio Hz and start time |
| Decoders | `HfdigFtx` (`hfdig_ftx.h`), `FtxDecode` has call, grid, cq, snr, dt, hz, slotUtc | Done |
| Band table | `core/include/dect2/bandplan/bandplan.h` (service "ham") | Channel plans, not used as a TX gate |

New in phase 1: `core/include/dect2/ham/ham.h` (+ `core/src/ham/ham.cpp`, test `tests/ham/test_ham_core.cpp`): amateur band table and `hamTxAllowed` gate, callsign/grid validation, ADIF writer/parser, `WorkedLog`, slot timing, message builders/parser, `QsoEngine` (CQ, reply, report, R-report, RR73, 73, retries, Hold, watchdog, halt). `app/ham/ham_ui.cpp`: the Ham panel (FT8/FT4/FT2).

## Phase 2 (done): the slot transmitter

`HamSlotTx` (core/src/ham/ham_tx.cpp, tested with a fake rig in tests/ham/test_ham_tx.cpp): gate `hamTxAllowed` again, modulate with `ftxAddTransmissionEx` at 48 kHz, PTT on, play, wait for the audio to drain, PTT off on every exit. `abort()` (Halt TX, disarm, watchdog) flushes the audio and releases PTT at once. The Ham panel runs it on a worker thread and calls `QsoEngine::txSent` only after the burst was really played. The real rig is Hamlib PTT (or none: VOX) plus a chosen sound card output and a level slider. Not done: SDR TX path (needs SSB IQ from the audio, via TxSession), RTS/DTR PTT, split, ALC/power meter, per-mode duty-cycle limit, mode sub-band check, in-app help. Touch interface has a Ham view (sheet button).

## Next phases

1. SDR transmit path for FT8/FT4/FT2 (audio to USB IQ through the TX sink, same gate).
2. Rig control: RTS/DTR PTT, split, frequency/mode set from the band choice, ALC and power warnings, sub-band check per mode, duty-cycle limit.
3. One mode per agent, in this order: WSPR beacon, JT65/JT9/Q65/FST4/MSK144 (engine variants), JS8Call, PSK31/RTTY/CW keyboard panel (shared), SSTV, APRS/packet, M17, then the rest of the table below.
4. Uploads, opt-in, user credentials, off by default: PSK Reporter, QRZ, LoTW, Club Log, eQSL.
5. Docs page "Operating with a rig" and in-app help.

## Modes (one agent per row)

| Mode | RX exists | TX exists (generator) | What Ham operation needs |
|---|---|---|---|
| FT8 | yes | yes (Ftx) | Phase 1 engine/panel done; phase 2 slot transmitter |
| FT4 | yes | yes | same, 7.5 s slots |
| FT2 | yes | yes | same, 3.75 s slots |
| WSPR | yes | yes | DONE (0.2.9): beacon in the Ham panel, `WsprBeacon` (core/src/ham/ham_wspr.cpp): 1 slot in N (N >= 2, default 5), power 0..60 dBm in WSPR steps, 1400..1600 Hz, stops after 30 transmissions without operator action; spots optional, not done |
| JT65 / JT9 / JT4 | yes | yes | DONE (0.2.9): the QSO engine runs them (60 s slots, 1 s start delay, RRR instead of RR73, JT65 A B C and JT4 A..G choice, mode bandwidth in the TX gate); test tests/ham/test_ham_modes.cpp (engine - HamSlotTx - decoder loopback). Known: the JT65 B / C decoder misses signals above about +5 dB |
| Q65 | yes | yes | engine variant, sub-mode + period choice |
| FST4 / FST4W | yes | FST4 yes | engine variant; FST4W beacon like WSPR |
| MSK144 | yes | yes | meteor-scatter 15 s repeating sequences, fast-sequence engine |
| JS8Call | yes | no | modulator + directed-message engine |
| PSK31 | yes | no | modulator (varicode BPSK), keyboard QSO panel |
| RTTY | yes | yes | keyboard panel, macros, contest exchange |
| CW | yes | no | keyer/text-to-Morse, memories |
| Olivia / Contestia / MFSK / THOR / DominoEX | yes | no | modulators, keyboard panel (shared) |
| Hellschreiber | yes | no | modulator, keyboard panel |
| MT63 / FSQ / IFKP | yes | no | modulators, keyboard/selcall panel |
| SSTV | yes | yes | picture send/receive panel |
| HamDRM / FreeDV | yes | yes (generators) | digital voice: user-installed codec, PTT, mic path |
| ARDOP | yes | no | ARQ session (large; defer) |
| APRS (AFSK) / packet | yes | no | AX.25 modulator, beacon, message panel |
| LoRa APRS | yes | no | LoRa modulator (check legal use band) |
| D-STAR / DMR / Yaesu Fusion / M17 / NXDN | yes | DMR only | only where an open codec/stack exists; M17 first (open) |
| HF ALE | yes | no | defer |
| QO-100, satellites | yes | no | uplink path via SDR TX; separate task |

## Safety rules used

Licence statement stored in prefs (`ham.licence`), callsign and grid validated, explicit Arm button (never saved; cleared by watchdog), frequency + bandwidth wholly inside an amateur band, Halt TX, watchdog (360 s without operator action), retry limit, no uploads.
