// The app side of user-installed speech codecs (see speech_codec_ui.h and docs/CODECS.md).
#include "speech_codec_ui.h"
#include "../app.h"
#include "../platform/platform.h"
#include "dect2/codec/speech_codec.h"
#include "dect2/engine/updater.h"
#include <filesystem>
#include <cstdio>
#include <chrono>
#include <map>
#include <set>

namespace appcodec {

namespace {

// Codecs whose notice the user chose "Not now" for in this run: the dialog is not reopened until the next start.
std::set<std::string> gDismissed;
std::map<std::string, bool> acceptedAt;

std::string prefKey(const std::string& id) { return "codec_notice_" + id; }

} // namespace

std::string codecsFolder() { return plat::dataDir() + "/codecs"; }

dect2::codec::CodecLookup speechCodecPoll(const char* id, std::unique_ptr<dect2::codec::SpeechCodec>& out) {
    const std::string codec = id;
    // Not ready yet: look again at most once a second (the plug-in file may be dropped in while running), and keep the last verdict between looks.
    static std::map<std::string, dect2::codec::CodecLookup> last;
    static std::map<std::string, double> lastAt;
    static std::map<std::string, std::string> lastLogged;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto it = last.find(codec);
    const bool accepted = plat::basePrefs().getB(prefKey(codec).c_str(), false);
    if (it != last.end() && it->second.status != dect2::codec::CodecStatus::Ready && now - lastAt[codec] < 1.0 && accepted == acceptedAt[codec])
        return it->second;
    acceptedAt[codec] = accepted;
    lastAt[codec] = now;
    const dect2::codec::CodecLookup r = dect2::codec::loadSpeechCodec(codec, codecsFolder(), accepted, out);
    last[codec] = r;
    // Log the reason whenever it changes, so a user with no audio can send the line.
    if (lastLogged[codec] != r.message) {
        lastLogged[codec] = r.message;
        fprintf(stderr, "[codec %s] %s\n", id, r.message.c_str());
    }
    return r;
}

bool speechCodecGate(const char* id, std::unique_ptr<dect2::codec::SpeechCodec>& out) {
    const std::string codec = id;
    const std::string folder = codecsFolder();
    const dect2::codec::CodecLookup r = speechCodecPoll(id, out);

    if (r.status == dect2::codec::CodecStatus::Ready) return true;

    if (r.status == dect2::codec::CodecStatus::NotInstalled) {
        ImGui::TextWrapped("%s", r.message.c_str());
        if (ImGui::Button("Open codecs folder")) {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(folder), ec);
            dect2::openUrl(folder);
        }
        ImGui::SameLine();
        if (ImGui::Button("Full instructions")) dect2::openUrl("https://github.com/adswill/OnAir/blob/main/docs/CODECS.md");
        return false;
    }

    if (r.status == dect2::codec::CodecStatus::NoticeRequired) {
        const std::string popup = "Speech codec notice: " + codec;
        if (!gDismissed.count(codec) && !ImGui::IsPopupOpen(popup.c_str())) ImGui::OpenPopup(popup.c_str());
        if (ImGui::BeginPopupModal(popup.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
            ImGui::TextWrapped("%s", dect2::codec::speechCodecNotice().c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::Button("I accept")) {
                plat::basePrefs().setB(prefKey(codec).c_str(), true);
                plat::basePrefs().flush();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Not now")) {
                gDismissed.insert(codec);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::TextWrapped("%s", r.message.c_str());
        return false;
    }

    // LoadFailed: say why, so the user can fix the file.
    ImGui::TextWrapped("%s", r.message.c_str());
    return false;
}

} // namespace appcodec
