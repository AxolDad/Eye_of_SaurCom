# Network radios: where they are saved, and recovering from a hang

Radios added by address (SoapyRemote, rtl_tcp, SpyServer, KiwiSDR, Pluto, ...) are saved in the settings file, in the line `netRadios=...`
(entries separated by `;`). The file is `settings.conf` in:

- macOS and Linux: `$XDG_CONFIG_HOME/zorcom/` or `~/.config/zorcom/`
- Windows: `%APPDATA%\ZorCom\`

If Eye of ZorCom hangs at start-up because of a saved network radio:

1. Start it once with `--safe-start`: no radio is chosen or opened by itself.
2. Close Eye of ZorCom, open `settings.conf` in a text editor, delete the entry you do not want from the `netRadios=` line (or the whole line), save.

Looking up a SoapyRemote radio runs in the background (it gives up after a few seconds); press the rescan button to see its radios once the answer is in.

## TCI (network), receive only

TCI is the Transceiver Control Interface of Expert Electronics' ExpertSDR3 (SunSDR radios); other programs offer a TCI server too. Add it in
**Network radios** with type **TCI (network)**: the address of the computer running the TCI server and port 50001 (the default), after turning the
server's TCI option on. Eye of ZorCom connects with a WebSocket (`ws://host:50001`), reads the server's start-up commands (device, protocol, `vfo_limits`,
`trx_count`, ... `ready`), then sets the IQ sample rate (48, 96, 192 or 384 kHz: the smallest at or above what the mode needs), puts the receiver's
DDS (panorama centre) and VFO on the tuned frequency and starts the IQ stream (`iq_start`) of one receiver (the **Receiver** setting appears when the
server has more than one). Float32, 24-bit and 16-bit samples are all understood. The frequency range comes from `vfo_limits`. TCI has no RF gain for
the receiver, so the gain slider does nothing here; the server's AGC stays as it is.

- Eye of ZorCom never transmits through TCI: it sends only `iq_samplerate`, `dds`, `vfo`, `iq_start` and `iq_stop`, and no audio or CW command.
- When the connection drops, Eye of ZorCom reconnects by itself and counts the samples lost.
- Tested only against a fake server that follows the TCI 1.9 specification. Unverified without a real ExpertSDR3: the URL path, whether the server
  accepts the VFO being moved with the DDS, and the exact meaning of `length` in the block header (the payload size is used).
