# Rewind (time machine)

The "Rewind" button in the diagnostics row keeps the last seconds of the radio's raw samples in memory, so the signal from just *before*
something happened can be saved as well as what comes after it.

* **Memory only.** The ring is 8-bit I/Q (2 bytes per sample) in RAM. "Reaches back" and "Memory limit" set its size; the memory limit wins, and
  the window shortens to fit (the window is shown). 10 Msps at 512 MB reaches back about 25 s.
* **Samples kept.** 1/2 ... 1/16 averages every 2 ... 16 samples before storing: the window is that many times longer, but only the middle
  part of the band (1/N of it) survives, so use it for a channel at the centre.
* **A save** writes `zorcom_rewind-<mode>_<MHz>_<rate>_<time>.cs8` (plus a `.sigmf-meta` and `.sigmf-data` link when SigMF is on) into the
  recordings folder: "Save the last (s)" before the trigger and "Then keep recording (s)" after it. The slider's bar shows how much history there is
  and how much of it a save keeps. A file is never bigger than 4 GB, and a save is refused if the disk has less room than the file needs.
* **Triggers.** The "Save now" button; the General receiver's squelch opening (tick the box); an automation rule with the action
  **Save the rewind** (seconds to keep after the event; any event can be the rule's trigger: a decode, a pass, an aircraft), or `save_rewind(seconds)` in a script.
* **No loss on the live path.** The analysis thread only converts a block and copies it into the pre-allocated ring (no lock held while
  copying, no allocation, no waiting for the disk); a separate thread copies the window out and writes the file. A retune empties the history
  (it would be at another centre frequency). A file source is not recorded.
* Applies to every mode: it taps the radio's samples before any mode-specific processing.
