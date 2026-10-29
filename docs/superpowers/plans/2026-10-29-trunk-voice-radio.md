# Trunking: voice on a second radio

**Goal:** One radio stays on the trunked system's control channel. Calls are followed on other radios. Setting this up takes one click: **Use a second radio for voice**.

**Why:** the user runs several SDRs for trunking. Today the follower can only hop its own radio off the control channel (`Retune`), or open a slice inside a wide capture (P25 only). While it hops, it misses grants.

**Scope:**
- P25 Phase 1, DMR Tier III and NXDN Type C. These share `trunk::Controller` in `core/src/p25/trunk_follow.cpp` and the global `Trunk T` in `app/p25/p25_ui.cpp`.
- Out of scope:
  - P25 Phase 2 voice, which has no demodulator;
  - EDACS, LTR and MPT1327, which decode the control channel only.

## Global constraints

- Never open or retune a radio that another receiver tab is using for something else. Respect `device_claims` and `radioTaken`.
- The control receiver never leaves the control channel while voice radios are linked.
- The voice receivers do not draw the mode screen, so the shared `static State S` of the P25, DMR and NXDN screens is never touched from two receivers.
- Untrusted text, such as talkgroup aliases and radio names, is drawn with `%s`. UI strings go through `tr()`, and all 12 catalogs stay consistent (`test_i18n`).
- Commits are made as AxolDad, with no attribution lines.

## Review focus

1. A grant arrives while every voice radio is busy. Priority preemption decides the outcome, as it does for slices. A lower-priority grant is logged as "missed (all voice radios busy)".
2. A voice radio is unplugged during a call. The call ends, the radio is unlinked, and the user is told. The control receiver is unaffected.
3. The user closes the voice tab or starts another mode on it. It is unlinked cleanly, and the follower falls back to its old behaviour.
4. The audio comes from the voice radio during a call and goes back afterwards. There are never two audio streams at once.
5. On shutdown, linked receivers stop in a safe order.

### Task 1: The follower routes grants to voice radios (core)

**Files:** `core/include/dect2/p25/trunk_follow.h`, `core/src/p25/trunk_follow.cpp`, and `tests/p25/test_p25_trunk.cpp` (or the existing trunk test).

- [ ] Add `setVoiceRadios(int n)`.
  - When n > 0, a grant outside the capture gives `Action::Retune` with `radio = k`, where k is a free voice radio.
  - The control state stays in ControlHold. It never goes to Hopped or Returning.
  - The call ends on hang time or on the terminator, as now. Ends are reported per voice radio.
  - Priority preemption picks the voice radio to take over: the one carrying the lowest-priority call.
  - When n = 0, the behaviour is exactly as today.
- [ ] **Tests:**
  - grants go to radio 0, then radio 1;
  - a third grant with both radios busy is preempted or missed according to priority;
  - the control channel is never left;
  - n = 0 behaves as before (the existing tests still pass).

### Task 2: The linked voice receiver, and the one-click setup (app)

**Files:**
- `app/state/receivers.cpp`/`.h`: link a receiver as "voice for RX n", and let `receiverModeBusy` allow it.
- `app/p25/p25_ui.cpp`: `nowView` applies `Retune{radio}` to the linked receiver, and reads that receiver's `rx.p25.now`, `rx.dmr.slot[]` and `rx.nxdn.now` for voice and terminators.
- The audible receiver (`gAudio`).
- The Follow tab: the button, and the "Voice: RX 2 · <radio>" line with an Unlink control.

- [ ] Add **Use a second radio for voice**.
  - It opens a receiver on the first free radio, links it, starts it in the same standard, and shows its state.
  - Pressed again, it adds a third radio.
  - With no free radio, it shows the same banner text as a new tab.
- [ ] A linked voice receiver shows a small panel in place of the mode screen: "Voice for RX 1 · TG 1234 · 851.0125 MHz", with an Unlink button.
- [ ] Cover Review Focus 2 to 5.
- [ ] Add an RF test with two engines on synthetic P25, a control channel plus a voice channel, in the style of `test_multi_rx`, if the synthetic P25 sources make this practical. Otherwise test the routing at the app-state level, and say which approach you took.

### Task 3: Docs

- [ ] In `docs/trunking.md`, replace "not supported yet" with how to use a second radio, its limits, and what happens when every voice radio is busy.
