// Mesh send (makeMeshMessageSynth): your own text goes out as one real Meshtastic packet (default LongFast channel) or MeshCore Public
// channel message, and the receiver decodes exactly that text, from the sender's name, with no CRC errors.
#include "dect2/mesh/mesh_gen.h"
#include "dect2/mesh/mesh_proto.h"
#include "dect2/mesh/mesh_rx.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;

#include "../common/test_util.h"

static MeshTelemetry run(int protocols, const std::string& text, const std::string& sender, double secs) {
    const double rate = 2.4e6;
    SynthConfig cfg;
    cfg.mode = 22; cfg.modeOpt[0] = protocols;
    auto syn = makeMeshMessageSynth(cfg, rate, text, sender);
    MeshTelemetry t;
    CHECK(syn != nullptr, "synth");
    if (!syn) return t;
    MeshReceiver rx;
    rx.configure(rate);
    if (protocols == 2) rx.setTunedHz(meshcoreDefaults("EU").freqHz);   // sent on the frequency the transmitter is tuned to: MeshCore's channel
    rx.setSignalOffset(-meshTuning().tuneOffsetHz);
    std::vector<cf32> buf(65536);
    for (size_t done = 0; done < (size_t)(secs * rate); done += buf.size()) { syn->generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    rx.telemetry(t, 0);
    return t;
}

static bool prefixHeard(const MeshTelemetry& t, const std::string& text) {
    for (const auto& m : t.messages) if (!m.text.empty() && text.compare(0, m.text.size(), m.text) == 0) return true;
    return false;
}

static bool heard(const MeshTelemetry& t, const std::string& text) {
    for (const auto& m : t.messages) if (m.text.find(text) != std::string::npos) return true;
    return false;
}

int main() {
    const std::string msg = "SaurCom test 73, ünïcödé ok";
    MeshTelemetry t = run(1, msg, "ZOR1 Base", 4);
    CHECK(heard(t, msg) && t.blocksBad == 0, "Meshtastic: the text decodes (%zu messages, %llu bad)", t.messages.size(), (unsigned long long)t.blocksBad);
    bool named = false;
    for (const auto& n : t.nodes) named |= n.id.size() > 1;
    CHECK(!t.nodes.empty() && named, "the sending node is listed");
    // a later run with the same name is the same node: same "!xxxxxxxx" id
    MeshTelemetry t2 = run(1, "again", "ZOR1 Base", 2);
    CHECK(!t.messages.empty() && !t2.messages.empty() && t.messages[0].from == t2.messages[0].from, "stable node id for the same name");

    MeshTelemetry c = run(2, msg, "ZOR1 Base", 4);
    CHECK(heard(c, msg) && c.blocksBad == 0, "MeshCore Public channel: the text decodes (%zu messages)", c.messages.size());

    // text too long for one packet is cut (not dropped): a non-empty prefix of it is sent
    MeshTelemetry l1 = run(1, std::string(255, 'a'), "ZOR1 Base", 4);
    CHECK(prefixHeard(l1, std::string(255, 'a')), "Meshtastic: 255 ASCII chars -> a prefix is heard (%zu messages)", l1.messages.size());
    std::string two;
    for (int i = 0; i < 200; i++) two += "\xC3\xA9";
    MeshTelemetry l2 = run(1, two, "ZOR1 Base", 4);
    CHECK(prefixHeard(l2, two) && l2.messages[0].text.size() % 2 == 0, "Meshtastic: 200 two-byte chars -> a whole-character prefix is heard (%zu messages)", l2.messages.size());
    MeshTelemetry l3 = run(2, two, "ZOR1 Base", 4);
    CHECK(!l3.messages.empty() && !l3.messages[0].text.empty() && prefixHeard(l3, two) && l3.messages[0].text.size() % 2 == 0,
          "MeshCore: 200 two-byte chars -> a whole-character prefix is heard (%zu messages)", l3.messages.size());

    CHECK(makeMeshMessageSynth(SynthConfig(), 2.4e6, "", "x") == nullptr, "no text: nothing (the demo scene is used instead)");
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
