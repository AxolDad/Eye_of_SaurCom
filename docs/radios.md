# Telling radios apart

If you have more than one radio plugged in, especially two of the same kind, Eye of SaurCom helps you tell which is which.

## The radio list

Each radio in the list shows its model, serial number and USB port, for example `RTL-SDR · SN 00000123 · USB 1-2`. Radios that look the same (same model and same serial) get `#1`, `#2` and so on, numbered by USB port so the numbers stay put.

Many dongles ship with the same serial (`00000001`). When two radios share a serial, a warning appears next to the list, with a button (**Fix…**) that opens the unique-serial tool described below.

## Naming a radio

Give a radio your own name and the list shows that instead of the model and serial. There are three ways:

- Click the **Aa** button next to the radio list.
- Right-click a radio in the list and choose **Name this radio…**.
- Type in the **Name** field in **Radio settings**.

A name can be up to 64 characters. **Clear name** in the naming window takes it off again.

Names are saved in `radios/names.json` inside the data folder:

| System | Data folder |
|---|---|
| Linux | `$XDG_DATA_HOME/saurcom`, else `~/.local/share/saurcom` |
| macOS | `~/Library/Application Support/SaurCom` |
| Windows | `%LOCALAPPDATA%\SaurCom` |

If the environment variable `DECT2_DATA_DIR` is set, that folder is used instead.

**What a name follows.** A name follows the radio's serial, so it stays with the radio when you move it to another USB port. When two radios share a serial, that cannot work, so the name follows the USB port instead. If you named a radio while it was alone and then plug in a second one with the same serial, the name moves once, to the `#1` radio (the one whose USB port name sorts first). Giving each radio its own serial (below) ends this.

**A damaged names file.** If `names.json` cannot be read, it is not overwritten. It is kept as `names.json.bad-<time>` next to the original, the program starts with no names, and a line in the log says so.

## Find this radio

Click the magnifier next to **Rescan**. Unplug the radio you want, wait for the message, then plug it back in. When it returns it is selected in the list, unless that receiver is running or another receiver tab is using the radio: the window then names the radio and says to stop the receiver first. The window waits 60 seconds. It offers **Name it** once the radio is found.

Limits:

- Only USB radios are found. Network radios, Hamlib rigs and Soapy-over-IP radios are not.
- While radios are in use, the radio that came back may show without its serial. Stop the receivers and press **Rescan** to see it.

## New-radio notice

When you plug in a radio while the program runs, a notice appears with three choices: **Name it**, **Use in new receiver tab**, or **Dismiss**. While radios are in use, or when the new radio cannot be matched to one entry of the list, the notice offers only **Dismiss** and says what to do instead (for example, stop the receivers and press **Rescan**). Radios that were already plugged in when the program started do not trigger it, and nothing appears in the first 5 seconds after start.

## A unique serial for an RTL-SDR

Only RTL-SDR dongles (RTL2832U) can be changed. Open it from **Radio settings** (**Give this RTL-SDR a unique serial…**) or from **Fix…** on the warning. You pick or accept an 8-digit serial that no other attached radio has, and then:

1. It refuses if the radio is in use. Stop whatever is using it first.
2. It reads the dongle's EEPROM twice and checks both reads agree.
3. It saves a backup to `radios/eeprom-backup-<old serial>-<time>.bin` in the data folder.
4. It writes the new serial and reads the EEPROM back to check it.
5. It asks you to unplug the dongle and plug it back in. Do not unplug it while it is writing.

If the new serial does not fit, or the EEPROM is not one the tool understands, nothing is written.

**Restore from backup…** (in the same place) puts a backup back. It checks that the file is a 256-byte RTL-SDR image from the same kind of dongle, and saves a safety copy of what is on the dongle now before it writes.

**If a write fails:** the message gives the path of the backup. Leave the dongle plugged in, open **Restore from backup…**, and restore that file. Then unplug and replug the dongle.

**Several dongles with the same serial.** The tool needs the USB ports to tell them apart. If the radio list shows no port for them, unplug all but one before you start.
