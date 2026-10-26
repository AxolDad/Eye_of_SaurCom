# Live transcripts: airband and marine voice as text

> **Off until you turn it on, and no speech model is included.** Eye of SaurCom downloads one only when you click, or reads a file you put in the models folder.

Eye of SaurCom can turn what is said on a voice channel into text while you listen, keep it, and tie it to the aircraft on the map. The speech engine
(whisper.cpp, MIT) runs **on your computer**: the audio never leaves it. Open it from **View > Transcripts**. The airband screen shows the last
calls as **live captions**; **ADS-B** shows the calls that name the selected aircraft in a **Voice (radio calls)** section.

## Privacy: read this first

- What is said on public aviation radio is public. A private conversation is not. **Marine VHF, amateur, CB and every other mode are off** until
  you tick them in the Transcripts window (*Settings and speech model*). The airband channels are the only ones on by default, and only once you
  have turned **Transcribe speech** on.
- While transcription runs, a red dot and **Transcribing speech: ...** are shown at the top of the status bar, in every mode.
- The text is kept in your data folder (`transcripts/transcripts.jsonl`) for the number of days you choose (default 7, at most 20 000 entries).
  **Delete all ...** in the window empties it.
- Transcripts never leave your computer.
- You are responsible for following the radio-listening and privacy law where you live.

## Set up

1. **View > Transcripts**, then **Settings and speech model**.
2. Tick **Transcribe speech**.
3. Get a model, one of two ways:
   - **Download speech model:** `tiny.en` (75 MB), `base.en` (142 MB) or `small.en` (466 MB). A confirmation names the file and the address. Eye of SaurCom asks
     `huggingface.co/ggerganov/whisper.cpp` for that one file, sends nothing else, and checks the file against a SHA-256 it was built with; a file that
     does not match is deleted. Other languages: *Other languages (multilingual models)* has `tiny`, `base` and `small` without `.en`.
   - **Your own file:** press **Open models folder** and put a whisper.cpp `ggml-*.bin` there (the folder is `<data folder>/models`: on macOS
     `~/Library/Application Support/SaurCom/models`, on Windows `%LOCALAPPDATA%\SaurCom\models`, on Linux `~/.local/share/saurcom/models`). It appears in the
     model list within a few seconds. What such a file contains is your responsibility.
4. Choose the model and the language. **English** is the default; **Auto-detect** and other languages need a multilingual model.

Which model: `tiny.en` keeps up with real time on a laptop (about 10 times faster than real time on an Apple M-series CPU) and is the right choice for
clear airband speech. `base.en` and `small.en` mistake fewer words and need a stronger computer. The speed shows in the window (*speed ... x real time*).

## What it does

- Per channel, Eye of SaurCom cuts the audio into transmissions: a transmission starts when the level passes the gate (the airband squelch already gates it),
  keeps 0.35 s in front and 0.7 s behind, and ends when the speaker stops. Clicks shorter than half a second are dropped; a transmission is cut at 27 s.
- Each transmission goes to a queue (at most 6 waiting; **the oldest is dropped** when the speech engine is slower than the radio) and is transcribed on
  a background thread, never in the audio path. While the channels are quiet nothing runs, so it costs no battery. **CPU threads** limits the processor.
- The speech engine is given a hint for aviation (ICAO alphabet, "cleared", "runway", "flight level", airline radio names) or marine speech, and
  on top of it the names that are **actually on the air** (below).
- Mistakes of the engine on silence and noise are filtered: text it itself marks as "no speech", sounds such as `[Music]`, the stock phrases it makes up
  on static ("Thank you.", "Thanks for watching"), a phrase repeated over and over, and more characters than a person can speak in that time.
- **Callsigns** are read from the text: `Lufthansa one two three alpha` becomes `DLH123A`, `Speedbird 4 7 2` `BAW472`, `november one two three alpha bravo`
  `N123AB`, `hotel bravo alpha lima charlie` `HBALC`, and a written `DLH123` as it is. The table has about 150 airline radio names. A call is linked to
  an aircraft on the screen when the callsign or registration matches an ADS-B aircraft; if it matches none now, it still shows up in that aircraft's
  list when the aircraft appears later. The *Speaker* column ("ATC" when the text starts with the callsign, "Aircraft" when it ends with it) is a guess.

## What the speech engine is given

The engine does not read the sound you hear. A radio channel carries things that are not speech and that a speech engine will nevertheless try to turn
into words, so Eye of SaurCom conditions a separate copy for it. **Nothing you hear or record goes through this** — the chain that cleans up your listening is a
different one (`docs/audio-cleanup.md`), with different settings, for a different reason.

In *Settings and speech model*, under **What the speech engine is given**:

| Setting | What it does |
|---|---|
| **Keep 300 – 3400 Hz** | A 6th-order high pass at 300 Hz and a 4th-order low pass at 3400 Hz. Takes out CTCSS tones (67 – 254.1 Hz), DCS turn-off bitstreams (134.4 bps) and the discriminator hiss above the voice band. 6th order up, not 4th, because a 4th-order corner leaves the highest CTCSS tone only 6.4 dB down, which is not enough to stop a strong one holding the transmission gate open; 6th order puts it 9.1 dB down and the common 203.5 Hz tone 20 dB down. |
| **Mute signalling bursts** | MDC-1200 and single-tone key-ups, found by how much of a 15 ms frame sits on 1200 and 1800 Hz, and muted **before the speech detector sees them** — so a data burst can neither start a transmission nor be read as syllables. It must persist for three frames before the mute acts, because a held vowel can look like a tone for a frame or two; a burst is therefore muted from about 45 ms in, which is what matters, since 45 ms cannot open a transmission by itself. |
| **Tell the engine what is on the air** | Below. |

Everything here is deterministic — fixed filters and a tone detector, no model and no training — and with it switched off the samples reach the engine
bit for bit as the radio produced them. There is deliberately **no noise gate for squelch tails**: the transmission gate already tracks the noise floor
and will not open below it, and a squelch tail sits at the noise floor by definition.

## Telling the engine what is on the air

A speech engine guesses from what it has heard before, and it has never heard of the aircraft overhead this minute. It will turn *Ryanair three four
alpha* into "Ryan air 34 alpha". Giving it the names that are genuinely in the air fixes most of that, and costs nothing.

Once a second Eye of SaurCom collects:

- the **call signs of the aircraft in range**, said the way they are said on the radio (`BAW472` → "Speedbird 472"), and their **registrations**;
  an aircraft close to you counts for more than one at the edge of coverage
- the **runways of the airports within 60 km**, spelled out (`25L` → "runway two five left"), and the airport names
- the **talkgroup names** of a trunked system you are following, from your own talkgroup list, with the one in a call now counting for more

They are ranked by how recently each was seen and how much it is worth, and cut to a character budget, because the engine reads only 224 tokens of hint
and the aviation phraseology above comes first. A name not seen for ten minutes is no longer "on the air" and drops out. The window shows what is
currently being offered, so the setting is not a matter of faith.

**Only names are used, never anything heard.** This reads the aircraft table, the airport database and your talkgroup list; nothing that was said goes
into it. It is off for auto subtitles, where the live names of aircraft have nothing to do with what is on television.

## The window

Search the text (all words must be found), filter by frequency, aircraft (callsign, registration or ICAO address) or airport (a code or name in the
channel name or the text). Click a line to copy it. **Export TXT / SRT / JSON / CSV** save the lines shown (SRT times are offsets from the first line;
CSV opens in Excel as UTF-8). **Copy all shown** puts the text on the clipboard.

## Live translation (optional)

With a **multilingual** model (tiny, base or small without `.en`) tick **Translate to English** in the Transcripts settings. Speech that is not English is
transcribed in its own language and then translated to English by the same model (whisper's "translate" task), offline. The Transcripts window and the
aircraft voice log show the original and the English line together; the detected language is stored with each entry, and the TXT, SRT (a second
`[EN]` line per cue), JSON and CSV (columns `language`, `text`, `translation`) exports carry both. Search also looks in the translation.

- It is a second pass over each foreign-language transmission, so it costs about 1.5 times the processor time. English speech is not translated.
  **Performance guard:** when the engine's average speed is already slower than 0.8 x real time, the translation pass is skipped (the original is still
  kept) and the skip is counted.
- English-only models ignore the option.
- **Out of scope:** translation into languages other than English. Nothing is ever sent to a cloud service.

## Auto subtitles (captions for the sound you hear)

Captions made from speech, for the receiver you are listening to: FM and AM broadcast, DAB, DRM, HD Radio, the audio of TV (DVB-T/T2/S/C, ATSC,
ISDB-T, DTMB, analogue, MAC), amateur voice (SSB/FM, QO-100), and the digital voices (DMR, NXDN, P25, dPMR, M17, D-STAR) when you have installed their
voice codec. Marine VHF and airband keep their own transcripts (above). **Encrypted traffic is never decoded.** It uses the same offline speech
engine and model; nothing is sent anywhere, and nothing is written to the transcripts file.

- **Off until you turn it on:** *View > Transcripts > Settings > Auto subtitles (captions) > Make captions*, then tick the modes. In the TV panel
  and next to the audio controls there is also an **auto captions** tick for the mode you are in.
- **Where they show:** over the picture for TV modes; in a small caption window (movable, closes = that mode off) for the radio modes. The window
  has **Save SRT / Save VTT**.
- **Style:** size (60-250 %), background opacity, 1-4 lines, top or bottom. **Caption text:** as spoken, English translation, or both (the translation
  is the live translation above and needs a multilingual model).
- **Language:** the language setting above; **Auto-detect** needs a multilingual model.
- **Timing:** the speech is cut at pauses, split into sentences, and each sentence is timed on the **audio clock** (samples played, not the wall
  clock), spread over the speaking time in proportion to its length. A cue shows a few seconds after the words (the latency is the transmission length
  plus the engine time; pieces are at most 12 s here). Cues never overlap.
- **Music is skipped:** a piece whose level has no syllable rhythm (steady music, tones, hum) is not sent to the engine (*music skipped* counter).
  Speech over music still passes. Pieces shorter than 1.5 s are not judged.
- **CPU guard:** one worker, a queue of 3 pieces; when the engine is slower than the audio the **oldest** speech is skipped, never the newest, the
  window says *behind real time*, and the translation pass is skipped above 0.8 x. Use a smaller model or fewer CPU threads.
- **TV: broadcast, generated, both:** *Station's own subtitles* (DVB, teletext; generated ones are never drawn), *Generated from speech*, *Both*
  (generated ones go to the top so they never cover the station's), or *Generated where the station has none* (default). The station's own subtitles
  are never overwritten or replaced unless you choose *Generated from speech*.
- **With recordings:** with **Record audio** running, `name.srt` and `name.vtt` are written next to the recording when it stops (times start at the
  moment the recording started; for a gated or split recording the times follow the live sound, not the cut file).
- **Limits:** one receiver's sound at a time (the one set to be heard); captions are as good as the speech model (tiny is fast, small is accurate);
  burn-in into recorded video is not done.

## If something does not work

- **"No speech model yet":** download one or put a file in the models folder. **"cannot load the speech model":** the file is not a whisper.cpp ggml
  model (a Core ML, GGUF-for-llama or PyTorch file will not load), or it is damaged.
- **Nothing is transcribed on airband:** the channel must be in the sample band, the squelch must open on it, and **Airband channels** must be ticked.
  The airband screen shows the live captions; the window shows *Heard, kept, left out*.
- **Many "skipped (too slow)":** the processor cannot keep up with the number of channels; use a smaller model or fewer channels. The queue never
  grows without limit.
- **Other languages come out in English:** the loaded model is English only (`.en`).
- The Windows and Linux packages use no GPU; the build is plain CPU code, portable x86-64 or ARM.

## For developers

`core/src/transcribe`: `segmenter` (voice activity, pre-roll and hang, 16 kHz resampler, music-versus-speech rhythm test), `captions` (sentence cues on the audio clock, SRT/VTT, TV source choice, caption style, eligible modes), `transcriber` (queue, workers, hallucination filters),
`callsign` (phonetic and telephony reader), `transcript` (store, search, TXT/SRT/JSON/CSV, file with retention), `models` (list, pinned SHA-256,
downloader), `whisper_backend` (whisper.cpp behind `SpeechBackend`), `audio_tap` (the sound of the non-airband modes). The app side is
`app/transcribe`. Tests: `test_transcribe` (fake speech backend: segmentation, queue overflow, filters, callsigns, search, exports, files, model list)
and `test_whisper_model`, which needs a model: `DECT2_WHISPER_MODEL=/path/ggml-tiny.en.bin ctest -R whisper_model` checks that silence, noise and a
tone produce no text, and prints the speed. Models are never committed. The vendored sources are in `third_party/whisper` (see its
`README-SaurCom.md`); build without them with `-DDECT2_WITH_WHISPER=OFF`.
