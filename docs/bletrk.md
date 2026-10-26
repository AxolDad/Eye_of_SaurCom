# BLE tracker detector

The mode **BLE tracker detector** (IoT & sensors group) is built on the Bluetooth LE advertising receiver. It recognises item trackers from their public
advertisements, shows how long and how close each one stays with the receiver, and warns about a tracker that follows you. It is receive only: Eye of ZorCom never
connects to a tracker, sends to one or reads its owner's data.

## What is recognised

| Tracker | How |
|---|---|
| Apple Find My network accessories (AirTag and compatible accessories, also Apple devices in offline finding) | Apple manufacturer data (company 0x004C), type 0x12, length 0x19: the offline-finding frame an accessory sends when it has lost contact with its owner's devices. The status byte is shown, not interpreted. |
| Tile | service UUID 0xFEED or 0xFD84 |
| Chipolo | service UUID 0xFE33 |
| Samsung SmartTag | service data of UUID 0xFD5A (SmartThings Find) |
| Google Find My Device network tags | Eddystone service data (0xFEAA) with the frame type 0x40, or 0x41 for the unwanted-tracking protection mode |

An AirTag that is near its owner sends the same Nearby frames as a phone and cannot be told apart; it shows up as a tracker only in the separated state, which is the
one that matters for unwanted tracking. Public key bytes and any other payload are neither read nor kept.

## What is shown

- Trackers: kind, state (following you, separated from its owner, nearby, your own), how long it stays, the share of the seconds heard in which it was near, the averaged
  level and its trend (closer, steady, farther), the number of rotating addresses chained to it, and when it was last heard.
- A tracker changes its address every so often. A new advertiser of the same kind that appears within 20 s of another going silent, at a level within 8 dB, continues that
  tracker; two tags heard at the same time stay two trackers.
- The count of distinct trackers per kind in the last minute and in the 30-minute memory window (Receiver tab).
- Alerts: a tracker **follows** when it has stayed for the alert time (default 10 minutes, 1 to 60 in the tuner), was heard in at least 60 % of the minutes of that span and
  was near in at least half of the seconds it was heard. One alert is raised per tracker. **Mark as mine** lists a tracker without ever alerting for it.
- Levels are the radio's own (dBFS), not calibrated distances. *Near* is an averaged level above a threshold (default -60 dBFS, set in the tuner) that you adjust for your
  radio and antenna. The detector is meant for a receiver that travels with you; a fixed receiver reports a neighbour's tag that simply stays in range.

## Alerts and the watchlists

An alert is written to the log and sent to the automation engine as the event `ble.tracker` (fields `kind`, `summary`, `name`, `minutes`, `rssi_db`; no address). A watchlist of the
**Words** kind containing `tracker` (or `unwanted tracking`) matches it and uses the notification, sound, ntfy, Telegram or webhook channels of the watchlists; an automation rule on
`ble.tracker` works as well.

## Privacy

Identifiers are kept in memory for 30 minutes after a tracker was last heard, shown as the last two address bytes only, never written to a file or to the results file, and
removed by **Clear**. The events sent to the watchlists carry no address.

## Not verified

No real tracker was available. The frame layouts and the service UUIDs are from the public descriptions (Apple's Find My network accessory specification, the TU Darmstadt
research on Find My and AirGuard, the Bluetooth SIG assigned numbers and Google's Find My Device network documentation) and are checked only against the Eye of ZorCom test signal. Real
captures of an AirTag, a Tile and a SmartTag would be the most useful check; the thresholds for *near* and the rotation chaining need real walks.

## Test signal

Five simulated trackers (Find My frame, Tile, Chipolo, SmartTag, Google tag) at different levels and a phone that is not a tracker. `tests/bletrk` checks the advertisement
detection (including look-alikes), the book on synthetic hours (one alert after the alert time, none for far, patchy or own trackers, rotation chaining, forgetting), the live
receiver on the generated signal with the watchlist match, and the engine path.
