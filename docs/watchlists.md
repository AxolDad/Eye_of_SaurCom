# Watchlists and alerts

View > Watchlists. Make lists of what you want to hear about, and say how Eye of SaurCom tells you.

## Lists

| Kind | Entries | Heard by |
|---|---|---|
| Aircraft | ICAO hex (4840D6), registration (A6-EDA), call sign (KLM1023) | ADS-B, ACARS, VDL2, HFDL |
| Ships | MMSI, ship name, call sign | AIS, DSC |
| APRS call signs | LY1ABC (any SSID) or LY1ABC-9 (that SSID only) | APRS |
| Frequencies | 162.025 (MHz), 162025 kHz, 144-146 (a range), with a tolerance | any decoded event on that frequency |
| Words in messages | a word or phrase, not case sensitive | pagers, ACARS, APRS, AIS, LoRa and other decoded text |

One entry per line (commas and semicolons also separate). A trailing `*` matches the start. Registrations and call signs are compared without case, spaces and dashes. Encrypted and voice traffic is never looked at.

## Alerts

* Desktop notification and the banner in the window.
* Sound: three beeps or your own WAV file; follows Mute.
* Push: ntfy (server, topic, optional token), a Telegram bot (bot token and chat id), or a generic webhook (address, optional Bearer token). You supply the endpoint and token; they are stored in `watchlists/watchlists.json` in the data folder (readable by you only) and never written to a log, the history or the status line. The webhook gets JSON: `source, list, kind, matched, type, mode, ident, freq_hz, time, summary`.
* "Send a test alert" tries every channel that is switched on.

## Limits

* Most alerts per hour, all lists together (default 30). Alerts over the limit are listed in the history as held back.
* The same entry on the same station again only after N minutes (default 5).
* Quiet hours (local time of the computer, may run over midnight): nothing is sent, the history still lists what was heard.

The History tab shows the last 300 matches and what happened to each. The window needs no running receiver of its own: the lists watch every receiver's decoders.
