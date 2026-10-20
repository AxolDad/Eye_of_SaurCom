# Network radios: where they are saved, and recovering from a hang

Radios added by address (SoapyRemote, rtl_tcp, SpyServer, KiwiSDR, Pluto, ...) are saved in the settings file, in the line `netRadios=...`
(entries separated by `;`). The file is `settings.conf` in:

- macOS and Linux: `$XDG_CONFIG_HOME/onair/` or `~/.config/onair/`
- Windows: `%APPDATA%\OnAir\`

If OnAir hangs at start-up because of a saved network radio:

1. Start it once with `--safe-start`: no radio is chosen or opened by itself.
2. Close OnAir, open `settings.conf` in a text editor, delete the entry you do not want from the `netRadios=` line (or the whole line), save.

Looking up a SoapyRemote radio runs in the background (it gives up after a few seconds); press the rescan button to see its radios once the answer is in.
