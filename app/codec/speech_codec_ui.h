// The app side of user-installed speech codecs: the codecs folder, the accepted-notice flag in prefs, and the notice dialog a voice
// mode draws until its codec is ready. English only. See docs/CODECS.md.
#pragma once
#include "dect2/codec/speech_codec.h"
#include <memory>
#include <string>

namespace appcodec {

// The user folder the plug-ins are looked for in: the app data folder + "/codecs".
std::string codecsFolder();

// Tries to load the speech codec `id` without drawing anything, so a voice mode can call it from its per-frame tick and get audio whatever
// tab is shown. Retries a missing or failed plug-in at most once a second; logs the reason when it changes. `out` holds the codec when Ready.
dect2::codec::CodecLookup speechCodecPoll(const char* id, std::unique_ptr<dect2::codec::SpeechCodec>& out);

// Draws the state of the speech codec `id` inside a voice mode: the not-installed text (with an Open codecs folder button), or the
// legal notice dialog until the user accepts it once. Call every frame. Returns true when `out` holds a ready codec.
bool speechCodecGate(const char* id, std::unique_ptr<dect2::codec::SpeechCodec>& out);

} // namespace appcodec
