# Trunking follower (P25, DMR Tier III, NXDN Type C)

Receive only. Eye of SaurCom decodes a trunked system's control channel and can follow the voice calls it announces. Open the **P25** tab, **Follow**, and turn on **Follow calls**.

## What it does

- **System model.** From the control channel's TSBKs (TIA-102.AABC): WACN, system, RFSS, site, NAC, the channel identifier table (IDEN_UP, IDEN_UP_VU, IDEN_UP_TDMA), adjacent sites, secondary control channels (SCCB), and the control-channel candidates to try when it is lost.
- **Calls.** Group voice grants (new and update messages, including the explicit update), unit-to-unit grants. Each becomes a call-log entry with talkgroup, source unit, frequency, duration, and an encrypted flag.
- **Voice following.**
  - *Wide capture* (whole-span decoding on): when the granted channel is inside the captured span the sound moves to that carrier. Up to N calls (1 to 4, default 2) are followed at once, limited by CPU. When all are busy a higher-priority talkgroup preempts the lowest one.
  - *Voice radios* (see [Several radios](#several-radios)): one click links other radios; the control radio stays on the control channel and each call outside the capture goes to a free voice radio.
  - *Single radio*: the radio leaves the control channel for the call, and returns when the call ends (no voice for the hang time, or a terminator). It does not hop away from a call that is already followed, and never hops when "retune the radio" is off. In both cases the entry says **needs a wider capture or a second radio** and counts as not followed.
  - If the control channel is lost the radio tries the known candidates in turn.
- **Talkgroup list.** Name, tag, category, priority (1 highest, 0 none), lockout, listen, scan list. Lockout talkgroups are logged but not followed. With **scan list only** only listed talkgroups with Listen on (and in an enabled list, when any list is enabled) are followed. The list is saved to `trunk_talkgroups.csv` in the Eye of SaurCom data folder.
- **Call log** with CSV export, **now talking** table, per-call recording (WAV, gated) when the vocoder add-on is present.

## Encrypted calls

P25 service options bit `0x40`, or a talkgroup you marked with an `E` mode in the list, and DMR privacy flags: the call is logged as *encrypted* and is never followed, recorded, decoded or attacked. IDs and talkgroups are still shown.

## Voice

Voice sound needs the user-installed vocoder add-on (see [CODECS.md](CODECS.md)). Without it everything else works; recordings are skipped because there is no sound to save.

## CSV columns

Import reads the header row, case-insensitive, any column order: `Decimal` (also `DEC`, `TGID`, `Talkgroup`), `Hex`, `Alpha Tag` (`Name`), `Mode` (D, DE, T, A), `Description`, `Tag`, `Category` (`Group`), and the Eye of SaurCom extras `Priority`, `Lockout`, `List`, `Listen`. The usual RadioReference-style export (Decimal, Hex, Alpha Tag, Mode, Description, Tag, Category) imports as it is; without a header that order is assumed. Quoted fields, doubled quotes, LF/CRLF and a UTF-8 BOM are accepted. A row without a valid number (1 to 65535) is skipped and counted. Eye of SaurCom downloads no data: you bring the file.

## Several radios

With one radio the follower has to leave the control channel for each call, and it misses the grants sent while it is away. With a
second radio it does not:

1. Start the receiver on the system's control channel (P25, DMR or NXDN) and open **Follow**.
2. Press **Use a second radio for voice**.

That is all. Eye of SaurCom opens a new receiver tab on the first free radio that can tune to the system (a radio another tab is using,
or has chosen, is never taken), starts it in the same mode, links it, and turns on **Follow calls**. The Follow tab then shows
**Voice: RX 2 · NESDR SMArt v5** with an **Unlink** button. Press the button again to add a third radio (as many as there are free radios
and receiver tabs: four tabs in all, so three voice radios at most). If no radio is free it says why: none connected, every radio in use
by another tab, or none of the free ones can tune to the system. The button works on the receiver that runs on the control channel.

- **How calls are followed.** The control radio never leaves the control channel while voice radios are linked. A grant outside the
  capture goes to a free voice radio, which tunes to the call's channel. The call ends on its terminator, which the voice radio hears, or
  when nothing is heard for the hang time. For DMR only the Terminator with LC ends a call early: a fade or a data burst does not, the hang
  time does. The voice radio then waits, silent, for the next call where it is. Calls inside the capture of a wide-band
  control radio (whole-span decoding) are still followed there first.
- **When every voice radio is busy.** Priority decides, as for the calls inside a wide capture: a higher-priority talkgroup takes over the
  voice radio that carries the lowest-priority call, which ends ("preempted by a higher priority call"). Otherwise the new call is logged as
  **missed (all voice radios busy)** and counted as not followed.
- **Sound.** While you listen to the trunked system (its control receiver or a voice radio is the receiver you hear), the sound moves to the
  voice radio that carries the call you should hear (the highest priority, then the oldest), and goes back afterwards. Only one receiver plays
  at a time: the others are muted before the new one is turned on. If you chose to hear another tab, the follower leaves your choice alone.
  A voice radio plays only while it carries a followed call (and never when you muted the control receiver).
- **The voice tab.** It shows a small panel instead of the mode screen ("Voice for RX 1 · TG 1234 · 851.0125 MHz") with **Unlink**. The
  talkgroups, the call log and the settings stay on the control receiver's Follow tab.
- **When something changes.**
  - *A voice radio is unplugged, loses its radio, or you press Stop on its tab*: it is unlinked at once, its call ends (and its recording
    stops), its tab closes, and a notice says why. The control receiver goes on with the radios that are left.
  - *You close the voice tab*: it is unlinked; *you switch it to another mode*: it is unlinked first and then is an ordinary tab of that
    mode (two tabs still cannot run one mode). The follower uses the radios that are left, and with none it follows calls with its own
    radio again, as before.
  - *You close the control receiver, or switch it to another mode (P25, DMR and NXDN included)*: its voice radios are unlinked and closed.
  - *You turn Follow calls off, the scanner leaves the system, or the control receiver stops*: the calls on the voice radios end, the
    sound goes back, and the voice radios stay linked but silent until following starts again.
  - *You quit*: the follower lets go of its voice radios first (the calls end, a recording stops), then the voice radios stop, then the
    other receivers. Voice radios are not reopened at the next start: one click makes them again.
- **Only the control receiver drives the follower.** Control messages are taken from it alone (never from a voice radio), each receiver's
  messages are counted on their own, and a stopped receiver's last screen of data is never read again.
- **Limits.** Each voice radio follows one call (for DMR: the call's time slot). P25 Phase 2 voice is still not followed (no Phase 2
  demodulator); EDACS, LTR and MPT1327 decode the control channel only. Every radio costs CPU: watch the CPU meter in the receiver strip.

With a wide-band radio (HackRF, 8 to 20 Msps) you can also turn on whole-span decoding instead: every voice channel inside the capture is
followed without leaving the control channel. Extra radios can run other modes in their own receiver tabs meanwhile.

## Honest scope

- **P25 Phase 1 control channel**: implemented and tested with synthetic signals (through the real C4FM receiver, and as message logic). Not tested on a real system.
- **P25 Phase 2**: the control channel is the same TSBK format, and IDEN_UP_TDMA is parsed (only channel type 3, 2 slots per carrier, is placed; other types are shown as not placed). There is **no Phase 2 voice demodulator**, so Phase 2 TDMA voice grants are logged and not followed.
- **Message layouts** are written from the field layouts as understood from TIA-102.AABC, which is not available offline here; the explicit update (0x03) and SCCB (0x39) layouts in particular need a check against the document.
- **DMR Tier III**: the follow logic is shared with P25. The CSBK opcodes (`DmrOpcodes`: PV_GRANT 0x30, TV_GRANT 0x31, BTV_GRANT 0x32) and the grant layout (12-bit physical channel number, timeslot, late entry, emergency, offset, 24-bit target, 24-bit source) follow ETSI TS 102 361-4 as other decoders read it; not yet tested on a live system. The grants carry channel numbers only, so you supply the channel table (LCN to frequency) in the DMR tab. Connect Plus and Capacity Plus are manufacturer protocols and are not supported.
- **NXDN Type C**: the control channel's voice channel assignments (VCALL_ASSGN, message type 0x04, NXDN TS 1-D; layout checked against sdrtrunk): call control option (bit 7 emergency), call type, call option, 16-bit source, 16-bit destination, call timer, 10-bit channel number. As with DMR you supply the channel table (channel number to frequency) in the follow panel of the NXDN Site tab. Systems that use direct frequency assignment (DFA) put frequency numbers in the message instead and are not followed. Tested with the generated control channel (`test_nxdn_rx`, `test_p25_trunk`); not yet on a live system.
- **Scanning trunked systems**: in View > Scanner an entry can be marked *Trunked*. On it the scanner turns the follower on (fresh site state, same talkgroup list and channel tables), counts a followed call as activity instead of the carrier a control channel always has, holds through the call and its hang time, then moves on. One list can hold several trunked systems (P25, DMR, NXDN) and conventional channels; the follower runs on the one the scanner is on.
- **Site map**: the control channel carries no coordinates; `SystemModel::setSitePosition` takes them from a user table, but there is no map view yet.
- **Transcription hook**: not wired. The call log entries carry the recording path for it.
- Only the selected carrier's sound plays, so two simultaneous followed calls are decoded and logged but one is heard (the highest priority).
- The Follow tab's text is not translated yet (like the rest of the P25 tab).

## Tests

`test_p25_trunk` (system model, follow state machine, priority, lockout, scan lists, rotating control channel, voice radios: routing, preemption, "missed (all voice radios busy)", a radio removed during a call, no voice radios as before; CSV, encrypted handling, 20000 random messages and junk CSV), `test_p25_trunk_rf` (the synthetic control channel through the receiver into the controller), `test_p25_trunk_voice` (what a voice radio hears in P25, DMR and NXDN telemetry, stale reports ignored, a DMR fade not ending a call;
action order on preemption, a moved call, reset and shrinking with calls running, following switched off; the app's rules for who feeds the
follower, when a voice radio is unlinked and who may share a mode), `test_p25_trunk_voice_rf` (two P25 receivers: the control channel on one, the call followed on the other until its terminator, the control radio never retuned).
