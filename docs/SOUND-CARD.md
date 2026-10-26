# Sound card input (audio device)

Pick "Sound card: ..." as the source for audio that does not come from an SDR: a virtual audio cable (BlackHole, VB-Cable) carrying a WebSDR or another program's receiver audio, a radio's speaker or line output, or a receiver's 12 kHz IF. Issue #137.

## Settings

| Setting | Use |
|---|---|
| I/Q mode | I/Q stereo (left I, right Q: HDSDR / WebSDR cable), USB audio, LSB audio, Real (mono). For audio from a cable or a speaker use Real (mono) or USB audio. Eye of ZorCom warns when left and right are identical in the I/Q mode and offers Real (mono). |
| Channel | Left, right, or both mixed (the real modes). |
| IF centre | The audio frequency of the signal's centre in the card. It is shifted down to 0 Hz, so the decoder sees the signal at the dial frequency. Plain audio: 0. A 12 kHz IF: 12000. "Find centre" sets it from the strongest signal on the screen (halfway between the two tones of an FSK signal). |
| Audio range | A Real mode shows the whole card range, 0 Hz to half the sample rate, across the spectrum, with the scale in audio Hz from the dial. Display only; the decoders always get the IF centre at 0 Hz. |
| Swap sideband, Remove DC, Gain | Mirror the spectrum, remove the card's offset, scale the input. |
| Sample rate | 8 to 192 kHz. A rate that is lower than the mode's receiver needs (most modes take 250 kHz and up) is interpolated up to the next whole multiple; the spectrum then spans that rate, use the zoom. |
| Level | Bar with a red "Clipping" warning when a sample reached full scale in the last two seconds. |

## Dial frequency

The frequency field is the dial the decoders assume for the IF centre. For weather fax it is the suppressed carrier (the USB dial: 1.9 kHz below the 7880 kHz carrier of a station on 7880 kHz is 7878.1 kHz), IF centre 0: the tones 1500 to 2300 Hz lie above it. For a mode whose centre is its tone pair (NAVTEX, RTTY) set the IF centre to where the pair sits in the audio.

## What was wrong (weather fax from a virtual cable)

1. The engine placed the narrowband receivers where a radio puts a channel: the radio is tuned the mode's `tuneOffsetHz` (20 kHz for the marine receiver) above the dial, so the receiver looked 20 kHz off the audio. A sound card has no radio to tune off a DC spike, so the offset must not apply (it was already excluded for files).
2. A narrowband receiver needs a minimum sample rate (250 kHz for weather fax, NAVTEX, DSC, CW, PSK, FT8...); a sound card gives at most 192 kHz and the receiver silently ignored every block. The sound-card source now interpolates up to a whole multiple that reaches the mode's minimum.
3. The CoreAudio input asked a mono device for two channels and a rate the device did not run at; it now uses the device's channels and rate and converts (mono copied to both channels, polyphase resampler).
4. The live input was paced by the computer's clock, not the card's, so over time frames were dropped or zeros inserted. It now takes what the card has captured.

## Tests

`tests/audio/test_audio_engine.cpp` renders synthetic signals into WAV files and runs the engine on a file-backed sound card (never a real device): weather fax at 12, 44.1, 48 and 96 kHz, mono / left only / right only, DC offset, 40 Hz mistuning, 300 ppm clock error, noise, USB mode, audio-range view (spectrum peak in the right bin and the picture matching the chart), plus NAVTEX, RTTY, FT8, CW and PSK31, the centre finder, the clipping flag and the L = R hint. The `audio_pace` radio key plays the file faster than real time.
