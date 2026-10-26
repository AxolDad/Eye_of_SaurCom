// Live transcripts in the app (core/src/transcribe): the settings, the speech model (downloaded on request or put in the models folder), the taps on
// the airband channels and on the sound of the other modes, the Transcripts window, the live captions of the airband screen, the "Voice" section of
// an aircraft. docs/TRANSCRIBE.md.
#pragma once
#include "dect2/acars/acars_log.h"
#include <cstdint>
#include <string>
#include <vector>

struct App;
struct ImDrawList;
struct ImVec2;

namespace tx {

void tick(App& first);                       // once per frame (cheap: it works about once a second)
void shutdown();
bool& windowOpen();                          // Tools > Transcripts
void windowPanel(App& a);                         // the Transcripts window (list, search, filters, export, settings, speech model)
void captions(App& a);                       // the live captions strip at the top of the airband screen
// What was said to or about an aircraft, newest first (from the callsign or registration spoken, or the aircraft the call was matched to).
void voiceSection(const App& a, uint32_t icao, const std::string& callsign, const std::string& reg);
size_t voiceCount(uint32_t icao, const std::string& callsign, const std::string& reg);
// The visible indicator: true and the words while transcription is running ("Transcribing airband with tiny.en").
bool indicator(std::string& text);


// ---- Auto subtitles (docs/TRANSCRIBE.md): captions from the speech of the sound you hear, on a receiver's overlay
bool captionsActive();                       // switched on and a speech model is loaded
bool captionsWanted(int standard);           // on for this mode (switch, per-mode tick)
bool captionsIsTv(int standard);             // the captions go over the picture (the TV overlay), not into a window
void captionsWindow(App& a);                 // the caption window of the receiver being heard (not for TV modes); call once per frame
// Over the TV picture, drawn by the subtitle overlay. broadcastPresent: the programme has DVB / teletext subtitles.
void captionsOverlay(App& a, ImDrawList* dl, ImVec2 p0, ImVec2 size, bool broadcastPresent);
bool captionsShowBroadcast();                // false when the user chose generated captions only
void captionsControls(App& a);              // the small row in the TV panel: on for this mode, broadcast / generated / both
void captionsSettings(App& a);               // the section in the Transcripts window
void captionsRecordStart();                  // an audio recording started: its time 0 is now
void captionsRecordStop(const std::string& recordingPath);   // wrote nothing unless captions ran; saves name.srt and name.vtt next to the file

}  // namespace tx
