// The app side of user-installed speech codecs: the codecs folder, the accepted-notice flag in prefs, and the notice dialog a voice
// mode draws until its codec is ready. English only. See docs/CODECS.md.
#pragma once
#include "dect2/codec/speech_codec.h"
#include <memory>
#include <string>

struct App;

namespace appcodec {

// The user folder the plug-ins are looked for in: the app data folder + "/codecs".
std::string codecsFolder();

// Tries to load the speech codec `id` without drawing anything, so a voice mode can call it from its per-frame tick and get audio whatever
// tab is shown. Retries a missing or failed plug-in at most once a second; logs the reason when it changes. `out` holds the codec when Ready.
dect2::codec::CodecLookup speechCodecPoll(const char* id, std::unique_ptr<dect2::codec::SpeechCodec>& out);

// Draws the state of the speech codec `id` inside a voice mode: the not-installed text (with an Open codecs folder button), or the
// legal notice dialog until the user accepts it once. Call every frame. Returns true when `out` holds a ready codec.
bool speechCodecGate(const char* id, std::unique_ptr<dect2::codec::SpeechCodec>& out);

// Forgets the last verdict for `id` so the next poll looks in the folder again (the Rescan button).
void speechCodecRescan(const char* id);

// Call every frame from a voice mode: loads the plug-in `id` once it is installed and the notice accepted, and registers it in
// dect2::codec (codec_registry.h) so every receiver of that family decodes through it. True when registered.
bool voiceCodecTick(const char* id);

// The install helper: loaded / not found with the exact file name, the codecs folder path, Open codecs folder, Build steps and Rescan buttons.
void codecInstallHelper(const char* id);

// The "Voice codecs" window (Help menu): every voice codec OnAir knows, the modes that use it, whether it is open, patented (plug-in only,
// never shipped) or unsupported, and whether it is loaded or missing, with the install helper for each plug-in.
void voiceCodecsWindow(App& a);

} // namespace appcodec
