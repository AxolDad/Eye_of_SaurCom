#include "notices.h"
#include "../app.h"
#include <algorithm>

namespace {
struct Notice {
    unsigned id = 0;
    std::string text;
    std::vector<NoticeButton> buttons;
    double until = 0;      // ImGui time when it goes away by itself; 0 = never
    double secs = 0;       // as asked; the clock starts when it is first drawn
    float h = 60;          // its height as drawn last frame, for stacking
};
std::vector<Notice> gNotices;
unsigned gNextId = 1;
constexpr size_t kMaxShown = 5;
}  // namespace

void noticePush(const std::string& text, std::vector<NoticeButton> buttons, double seconds) {
    Notice n;
    n.id = gNextId++;
    n.text = text;
    n.buttons = std::move(buttons);
    if (n.buttons.empty()) n.buttons.push_back(NoticeButton{tr("OK"), nullptr, true});
    n.secs = seconds;
    gNotices.push_back(std::move(n));
    while (gNotices.size() > kMaxShown) gNotices.erase(gNotices.begin());
}

int noticeCount() { return (int)gNotices.size(); }

void noticesDraw(float bottomInset) {
    if (gNotices.empty()) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const double now = ImGui::GetTime();
    const float margin = 10 * gUi;
    float y = vp->Pos.y + vp->Size.y - bottomInset - margin;
    unsigned dismissId = 0;
    std::function<void()> run;
    for (Notice& n : gNotices) {
        if (n.secs > 0 && n.until == 0) n.until = now + n.secs;
        if (n.until > 0 && now >= n.until) { dismissId = n.id; continue; }
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - margin, y), ImGuiCond_Always, ImVec2(1.f, 1.f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(260 * gUi, 0), ImVec2(380 * gUi, 400 * gUi));
        ImGui::SetNextWindowBgAlpha(0.96f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * gUi, 8 * gUi));
        const std::string id = "##notice" + std::to_string(n.id);
        if (ImGui::Begin(id.c_str(), nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
            ImGui::PushTextWrapPos(340 * gUi);
            ImGui::TextUnformatted(n.text.c_str());   // text, never a format string
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            for (size_t i = 0; i < n.buttons.size(); i++) {
                if (i) ImGui::SameLine();
                const std::string bl = n.buttons[i].label + "##nb" + std::to_string(i);
                if (ImGui::Button(bl.c_str())) {
                    if (n.buttons[i].dismiss) dismissId = n.id;
                    run = n.buttons[i].onClick;   // run after the loop: it may push notices
                }
            }
            n.h = ImGui::GetWindowSize().y;
        }
        ImGui::End();
        ImGui::PopStyleVar();
        y -= n.h + 6 * gUi;
    }
    if (dismissId) gNotices.erase(std::remove_if(gNotices.begin(), gNotices.end(), [&](const Notice& n) { return n.id == dismissId; }), gNotices.end());
    if (run) run();
}
