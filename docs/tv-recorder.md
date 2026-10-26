# TV recorder: programme guide, recordings, timeshift, subtitles

Works on the DVB transport stream that Eye of SaurCom already decodes (DVB-T/T2, DVB-C, DVB-S/S2, ISDB-T, DTMB and the others that deliver an MPEG-TS). Everything is in the TV tab: **Player**, **Guide**, **Recordings**, **Teletext**.

## Legal note

Recording is for personal use. Scrambled (encrypted) services are recorded as they arrive and are never decoded; Eye of SaurCom has no decryption. A programme whose video Eye of SaurCom cannot decode (AVS+, AVS2/3, see CODECS.md) still records and still appears in the guide, and plays in other players that support the codec.

## Recordings run only while Eye of SaurCom runs

The recorder is part of the application. It records only while Eye of SaurCom is open, the computer is awake (a sleeping computer records nothing) and the receiver is tuned to the multiplex the programme is on. The "Tune to a scheduled recording's channel when it starts" option lets the recorder tune the receiver to a saved channel by itself; it does not wake the computer. A job whose window passes without the receiver on the right multiplex is marked **missed**, with the reason.

## Programme guide (EPG)

Source: EN 300 468 EIT tables on PID 0x12, present/following and schedule, of the tuned transport stream and of other ones ("other" tables), the TDT/TOT on PID 0x14 and the SDT for service names.

Per event the database keeps: title, short text and extended text (all descriptors joined, with the character table prefix of each text honoured: ISO 6937, 8859-x, UTF-8, UCS-2, GB 2312...), language, start and duration, running status, scrambled flag, genre (content descriptor), minimum age (parental rating), programme and series CRIDs (content identifier, TS 102 323).

* Per service, keyed by original network, transport stream and service id, so the guide survives retuning.
* Bounded: 3000 events per service, 400 services, nothing that ended more than 6 hours ago, nothing starting more than 21 days ahead (broken start times). An event of another id that overlaps a new one is dropped (the broadcaster rescheduled).
* Saved to `<data>/tv/epg.json` every 5 minutes and at exit, loaded at start; expired events are dropped on load.
* Time zone: times are shown in the broadcaster's zone from the TOT local time offset descriptor (including the summer time change, `time_of_change` / `next_time_offset`); without a TOT the computer's zone is used. Stream time comes from the TDT/TOT, advanced with the stream.

Guide tab: channels as rows, a 26 hour time grid (scroll, "Now", +-6 h, zoom), colour by genre, a red dot on programmes that will be recorded, the current time as a line. Click a programme for the details pane (description, genre, age, language). Search finds all words in title and description across all services. Double-click a programme of the tuned multiplex to watch it. Buttons: **Record**, **Record series** (needs a series CRID in the broadcast: the request then adds every future episode on that service, skipping repeats of an episode that is already scheduled or recorded), **Remind me**, **Watch**.

## Recordings

Recordings tab: **Scheduled** (jobs, state, conflicts, remove / stop), **Manual recording** (service, clock time, minutes), **Recorded** (play, export, delete, open folder, free space) and **Recorder settings**.

* A recording is one MPEG-TS file per service containing only that service (single-program PAT, PMT, PCR, video, audio, subtitle, teletext PIDs), so the files play in any player. The sidecar `<name>.ts.json` holds title, description, service, programme and actual times, size, dropped packets, status, error, the EPG entry and the subtitle files.
* **Several services of one multiplex record at the same time** (one PID filter per service over the same packet stream, one file each). Services on different multiplexes need one tuner each; the **Tuners** setting says how many multiplexes can be received at once (default 1). With one tuner the older request wins and an overlapping recording on another multiplex is marked as a conflict (shown in the guide and the list, and failed with a reason when its window opens).
* Padding: by default 1 minute before and 5 minutes after (settings).
* File names from a template: `{title} {service} {date} {time} {year} {month} {day} {id}`; characters not allowed on Windows, macOS or Linux are replaced, names are cut at 120 bytes on a character boundary, a taken name gets `_2`...
* Disk guard: a job does not start, and a running one stops, when less than the "Keep free" space (default 2 GB) is left. A disk too slow for the stream drops packets and the number is stored in the sidecar.
* Signal loss: a recording ends (marked failed, the data kept) after the set number of seconds without packets of its multiplex; 0 turns this off.
* The schedule is saved to `<data>/tv/schedule.json`. A job that was recording when the application stopped is marked failed ("the application stopped while recording"); the file is kept.
* Recordings made before this feature (plain `.ts` files in the recordings folder) are listed too.

## Timeshift

Settings: **Timeshift buffer on disk** and its length in minutes (default 30). While on, the playing service is written to a rolling buffer of segment files in `<data>/timeshift` (about 3 MB each); the oldest segment is deleted when the buffer holds more than the set time. The buffer is deleted when it is turned off and at exit.

On the Player tab: **Pause (timeshift)** freezes live TV; the buffer keeps filling. Play resumes where it stopped, **-30 s / -10 s / +10 s / +30 s** jump, **Live** returns to the live picture. Positions are mapped to time by the PCR of the service (unwrapped over the 26.5 hour rollover; a jump in the broadcaster's clock continues the timeline), playback is paced by the same clock. Fast-forward is the +10 / +30 s jumps; a faster-than-real-time picture with sound is not offered. Timeshift needs the receiver on the live service; with the multi-channel receiver it follows the selected channel.

## Playing recordings

Play on a recording opens the file (the whole file is scanned once for the time map: about a second per GB) and plays it through the same player as live TV, with pause, seek bar, +-10 / 30 s and Stop (back to live). Teletext subtitles and DVB subtitles of the recording are shown by the normal subtitle controls (language list of the recorded service); the subtitle decoders follow the recording while it plays.

## Subtitles

* Live and played: DVB subtitles (bitmap) and teletext subtitles, with the language list in the Player controls.
* Saved next to a recording (option on by default): **teletext subtitle pages** (type "subtitle" or "hearing impaired" in the teletext descriptor) as `<name>.<lang>.srt` and `.vtt`, one pair per language. Times are stream time from the start of the recording (PCR).
* Bitmap DVB subtitles are recorded inside the .ts (and shown on playback), but not converted to text; a PNG overlay export is not implemented.

## Not covered

* The multi-channel receiver delivers one selected channel to the recorder and the guide; recordings from the other channels of a wide capture need that channel selected.
* No wake-up of a sleeping computer, no remote scheduling.
* Services on a multiplex Eye of SaurCom has not been tuned to yet cannot be scheduled from the "other" EIT (the frequency is unknown); tune to that multiplex once and it is remembered (`<data>/tv/muxes.json`).

## Code

`core/src/tv/epg.cpp` (EIT / TOT parsing, database, collector), `recorder.cpp` (schedule, conflicts, naming, sidecars, recorder), `timeshift.cpp` (time map, disk buffer), `playback.cpp` (file source, pacer, feeder), `subtitle_export.cpp` (SRT / WebVTT, teletext cues); UI in `app/panels/tvrec_ui.cpp`. Tests: `test_tv_epg`, `test_tv_recorder`, `test_tv_timeshift` (tests/tv).
