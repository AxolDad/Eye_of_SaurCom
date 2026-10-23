// "Stream" next to the analog TV and FPV pictures: the picture as MPEG-2 in a transport stream over UDP (core/src/tv/video_stream.cpp).
// One streamer for both; the address and port are saved. VLC opens udp://@:<port> on the receiving computer.
#include "../app.h"
#include "dect2/tv/video_stream.h"
#include <cstdio>
#include <string>

namespace {
dect2::VideoStreamer& streamer() { static dect2::VideoStreamer v; return v; }
std::string gErr;   // why the last start failed
}

void videoOutPush(std::shared_ptr<const dect2::AtvFrame> f) { streamer().push(std::move(f)); }

void videoOutButton() {
    dect2::VideoStreamer& v = streamer();
    const dect2::VideoStreamStats st = v.stats();
    if (ImGui::SmallButton(st.active ? tr("Stop stream") : tr("Stream"))) {
        if (st.active) v.stop();
        else ImGui::OpenPopup("##videoout");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(st.active ? "%s: %llu pictures, %.1f MB" : "%s", st.active ? tr("Streaming the picture") : tr("Sends the picture over the network (MPEG-2 in MPEG-TS over UDP) for VLC, ffplay or OBS"),
                          (unsigned long long)st.framesSent, st.bytesSent / 1e6);
    if (ImGui::BeginPopup("##videoout")) {
        static char host[64] = "";
        static int port = 0;
        plat::Prefs& d = plat::prefs();
        if (!host[0]) { snprintf(host, sizeof host, "%s", d.getS("videoOutHost", "239.255.0.1").c_str()); port = (int)d.getI("videoOutPort", 5004); }
        ImGui::TextDisabled("%s", tr("To one computer (its address) or to every listener (a 239.x.x.x group)"));
        ImGui::SetNextItemWidth(150 * gUi); ImGui::InputText("##vhost", host, sizeof host);
        ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi); ImGui::InputInt("##vport", &port, 0, 0);
        port = std::max(1, std::min(65535, port));
        ImGui::TextDisabled(tr("Open on the other computer: udp://@:%d (or udp://@%s:%d for a group)"), port, host, port);
        if (ImGui::Button(tr("Start streaming"))) {
            dect2::VideoStreamConfig c; c.host = host; c.port = port;
            d.setS("videoOutHost", host); d.setI("videoOutPort", port);
            gErr.clear();
            v.start(c, gErr);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    const std::string& e = !gErr.empty() ? gErr : st.error;
    if (!e.empty()) { ImGui::SameLine(); ImGui::TextColored(pal::warnAmber(), "%s", e.c_str()); }
}
