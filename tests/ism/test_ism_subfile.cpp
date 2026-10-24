// Flipper Zero .sub files: the three kinds parse, the timings fold into the pulses the decoders want, a file written here reads back the
// same, and a test device's frame that went through a .sub file still decodes to the same reading, through the parser alone and
// through the receiver's import.
#include "dect2/ism/ism_gen.h"
#include "dect2/ism/ism_proto.h"
#include "dect2/ism/ism_rx.h"
#include "dect2/ism/ism_subfile.h"
#include "dect2/ism/ism_tel.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    // the RAW example of the Flipper firmware's documentation (SubGhzFileFormats.md): two positive timings in a row, a line starting
    // with a gap
    {
        const std::string text = "Filetype: Flipper SubGhz RAW File\nVersion: 1\nFrequency: 433920000\nPreset: FuriHalSubGhzPresetOok650Async\nProtocol: RAW\n"
                                 "RAW_Data: 29262 361 -68 2635 -66 24113 -66 11\nRAW_Data: -424 205 -412 159 -412 381 -240 181\n";
        SubFile f; std::string err;
        CHECK(parseSubFile(text, f, err), "RAW example: %s", err.c_str());
        CHECK(f.kind == SubFile::Raw && f.version == 1 && !f.fsk, "kind %d version %d fsk %d", (int)f.kind, f.version, f.fsk);
        CHECK(std::fabs(f.freqHz - 433.92e6) < 1, "freq %.0f", f.freqHz);
        CHECK(f.preset == "FuriHalSubGhzPresetOok650Async" && f.protocol == "RAW", "preset %s protocol %s", f.preset.c_str(), f.protocol.c_str());
        CHECK(f.values == 16, "%zu values", f.values);
        // 29262+361 | 68 ; 2635 | 66 ; 24113 | 66 ; 11 | 424 ; 205 | 412 ; 159 | 412 ; 381 | 240 ; 181 | (long)
        CHECK(f.train.p.size() == 8, "%zu pulses", f.train.p.size());
        if (f.train.p.size() == 8) {
            CHECK(f.train.p[0].pulseUs == 29623 && f.train.p[0].gapUs == 68, "p0 %.0f/%.0f", f.train.p[0].pulseUs, f.train.p[0].gapUs);
            CHECK(f.train.p[3].pulseUs == 11 && f.train.p[3].gapUs == 424, "p3 %.0f/%.0f", f.train.p[3].pulseUs, f.train.p[3].gapUs);
            CHECK(f.train.p[7].pulseUs == 181 && f.train.p[7].gapUs >= 20000, "last %.0f/%.0f", f.train.p[7].pulseUs, f.train.p[7].gapUs);
        }
    }
    // a key file: no timings, the protocol and the key as written
    {
        const std::string text = "Filetype: Flipper SubGhz Key File\nVersion: 1\nFrequency: 433920000\nPreset: FuriHalSubGhzPresetOok650Async\n"
                                 "Protocol: Princeton\nBit: 24\nKey: 00 00 00 00 00 95 D5 D4\nTE: 400\n";
        SubFile f; std::string err;
        CHECK(parseSubFile(text, f, err), "key file: %s", err.c_str());
        CHECK(f.kind == SubFile::Key && f.protocol == "Princeton" && f.bits == 24 && f.teUs == 400, "kind %d %s %d %d", (int)f.kind, f.protocol.c_str(), f.bits, f.teUs);
        CHECK(f.key == "00 00 00 00 00 95 D5 D4", "key '%s'", f.key.c_str());
        CHECK(f.train.p.empty(), "%zu pulses from a key file", f.train.p.size());
    }
    // a custom preset of an FSK kind, Windows line ends
    {
        const std::string text = "Filetype: Flipper SubGhz RAW File\r\nVersion: 1\r\nFrequency: 868350000\r\nPreset: FuriHalSubGhzPreset2FSKDev476Async\r\n"
                                 "Protocol: RAW\r\nRAW_Data: 500 -500 1000 -1000 500 -500\r\n";
        SubFile f; std::string err;
        CHECK(parseSubFile(text, f, err), "FSK RAW: %s", err.c_str());
        CHECK(f.fsk && f.train.fsk, "fsk %d", f.fsk);
        CHECK(f.train.p.size() == 3 && f.train.p[1].pulseUs == 1000 && f.train.p[1].gapUs == 1000, "%zu pulses", f.train.p.size());
    }
    // BinRAW: bits of TE each, 1 = carrier. 0xF0 0x0F at TE 100: 400 on, 800 off, 400 on
    {
        const std::string text = "Filetype: Flipper SubGhz RAW File\nVersion: 1\nFrequency: 433920000\nPreset: FuriHalSubGhzPresetOok650Async\n"
                                 "Protocol: BinRAW\nBit: 16\nTE: 100\nBit_RAW: 16\nData_RAW: F0 0F\n";
        SubFile f; std::string err;
        CHECK(parseSubFile(text, f, err), "BinRAW: %s", err.c_str());
        CHECK(f.kind == SubFile::BinRaw && f.values == 16 && f.teUs == 100, "kind %d values %zu te %d", (int)f.kind, f.values, f.teUs);
        CHECK(f.train.p.size() == 2, "%zu pulses", f.train.p.size());
        if (f.train.p.size() == 2) {
            CHECK(f.train.p[0].pulseUs == 400 && f.train.p[0].gapUs == 800, "p0 %.0f/%.0f", f.train.p[0].pulseUs, f.train.p[0].gapUs);
            CHECK(f.train.p[1].pulseUs == 400 && f.train.p[1].gapUs >= 20000, "p1 %.0f/%.0f", f.train.p[1].pulseUs, f.train.p[1].gapUs);
        }
    }
    // what is refused, and why
    {
        SubFile f; std::string err;
        CHECK(!parseSubFile("Filetype: Flipper NFC device\nVersion: 2\n", f, err) && err.find("Sub-GHz") != std::string::npos, "NFC file: '%s'", err.c_str());
        CHECK(!parseSubFile("hello\n", f, err) && !err.empty(), "junk: '%s'", err.c_str());
        CHECK(!parseSubFile("Filetype: Flipper SubGhz RAW File\nProtocol: RAW\nRAW_Data: 1 -1\n", f, err) && err.find("Frequency") != std::string::npos, "no frequency: '%s'", err.c_str());
        CHECK(!parseSubFile("Filetype: Flipper SubGhz RAW File\nFrequency: 433920000\nProtocol: RAW\n", f, err) && err.find("RAW_Data") != std::string::npos, "no data: '%s'", err.c_str());
        CHECK(!parseSubFile("Filetype: Flipper SubGhz RAW File\nFrequency: 433920000\nProtocol: BinRAW\nBit_RAW: 8\nData_RAW: FF\n", f, err) && err.find("TE") != std::string::npos, "BinRAW without TE: '%s'", err.c_str());
    }
    // every test device's frame: written as a .sub file, read back, decoded to the same model and ID (the writer rounds to whole
    // microseconds, which the decoders tolerate; the receiver measures in whole samples anyway)
    int rounds = 0;
    for (int p = 0; p < (int)IsmTx::Count; p++) {
        const IsmTx tx = (IsmTx)p;
        if (tx == IsmTx::ZwaveR1 || tx == IsmTx::ZwaveR2 || tx == IsmTx::ZwaveR3) continue;   // Z-Wave is not an OOK/FSK pulse protocol a Flipper captures
        const IsmTrain t = ismEncode(tx, 0);
        const auto want = ismDecode(t);
        if (want.size() != 1) continue;
        const std::string text = subFileText(t, 433.92e6);
        CHECK(text.rfind("Filetype: Flipper SubGhz RAW File\n", 0) == 0, "%s: header", ismTxName(tx));
        CHECK(text.find(t.fsk ? "Preset: FuriHalSubGhzPreset2FSKDev476Async" : "Preset: FuriHalSubGhzPresetOok650Async") != std::string::npos, "%s: preset", ismTxName(tx));
        SubFile f; std::string err;
        CHECK(parseSubFile(text, f, err), "%s: read back: %s", ismTxName(tx), err.c_str());
        CHECK(f.train.p.size() == t.p.size() && f.train.fsk == t.fsk, "%s: %zu pulses back of %zu", ismTxName(tx), f.train.p.size(), t.p.size());
        const auto got = ismDecode(f.train);
        CHECK(got.size() == 1 && got[0].model == want[0].model && got[0].id == want[0].id, "%s: %zu readings%s", ismTxName(tx), got.size(),
              got.empty() ? "" : (", " + got[0].model + " " + got[0].id).c_str());
        rounds++;
    }
    CHECK(rounds >= 15, "%d devices round-tripped", rounds);
    // a line holds at most 512 timings
    {
        IsmTrain t;
        for (int i = 0; i < 600; i++) t.p.push_back({300, 300});
        const std::string text = subFileText(t, 433.92e6);
        size_t lines = 0, longest = 0, pos = 0;
        while (pos < text.size()) {
            size_t e = text.find('\n', pos);
            if (e == std::string::npos) e = text.size();
            if (text.compare(pos, 9, "RAW_Data:") == 0) {
                lines++;
                size_t n = 0;
                for (size_t i = pos; i < e; i++) if (text[i] == ' ') n++;
                longest = std::max(longest, n);
            }
            pos = e + 1;
        }
        CHECK(lines == 3 && longest == 512, "%zu RAW_Data lines, longest %zu values", lines, longest);
        SubFile f; std::string err;
        CHECK(parseSubFile(text, f, err) && f.train.p.size() == 600, "600 pulses: %zu back", f.train.p.size());
    }
    // the receiver's import, with the receiver never started: the device shows up in the published telemetry, the log names it, the
    // summary says what was decoded; a train no decoder takes goes under the unknown signals; a key file is logged
    {
        IsmReceiver rx;
        IsmTelemetry tel;
        CHECK(!rx.telemetry(tel, 0), "telemetry before anything happened");
        const IsmTrain t = ismEncode(IsmTx::Nexus, 0);
        const auto want = ismDecode(t);
        std::string summary, err;
        CHECK(rx.importSubFile(subFileText(t, 433.92e6), summary, err), "import: %s", err.c_str());
        CHECK(rx.telemetry(tel, 0), "telemetry after the import");
        CHECK(tel.devices.size() == 1 && !want.empty() && tel.devices[0].model == want[0].model && tel.devices[0].id == want[0].id, "%zu devices%s", tel.devices.size(),
              tel.devices.empty() ? "" : (": " + tel.devices[0].model).c_str());
        CHECK(!tel.devices.empty() && std::fabs(tel.devices[0].freqHz - 433.92e6) < 1, "device at %.0f Hz", tel.devices.empty() ? 0.0 : tel.devices[0].freqHz);
        CHECK(tel.events.size() == 1 && tel.blocksOk == 1, "%zu events, %llu ok", tel.events.size(), (unsigned long long)tel.blocksOk);
        CHECK(summary.find("433.920 MHz") != std::string::npos && !want.empty() && summary.find(want[0].model) != std::string::npos, "summary '%s'", summary.c_str());
        printf("  import: %s\n", summary.c_str());
        // an unknown signal: a PWM-looking train no protocol accepts
        IsmTrain u;
        for (int i = 0; i < 40; i++) u.p.push_back({i % 3 ? 250.f : 750.f, i % 3 ? 750.f : 250.f});
        u.p.back().gapUs = 50000;
        const uint64_t seq = tel.seq;
        CHECK(rx.importSubFile(subFileText(u, 868.3e6), summary, err), "import unknown: %s", err.c_str());
        CHECK(rx.telemetry(tel, seq), "telemetry after the unknown import");
        CHECK(tel.raws.size() == 1 && tel.blocksBad == 1 && tel.events.size() == 2 && tel.events[0].unknown, "%zu raws, %llu bad, %zu events", tel.raws.size(),
              (unsigned long long)tel.blocksBad, tel.events.size());
        CHECK(summary.find("no protocol matched") != std::string::npos && summary.find("868.300 MHz") != std::string::npos, "summary '%s'", summary.c_str());
        printf("  import: %s\n", summary.c_str());
        // a key file
        CHECK(rx.importSubFile("Filetype: Flipper SubGhz Key File\nVersion: 1\nFrequency: 433920000\nPreset: FuriHalSubGhzPresetOok650Async\nProtocol: Princeton\nBit: 24\nKey: 00 00 00 00 00 95 D5 D4\nTE: 400\n", summary, err), "key import: %s", err.c_str());
        CHECK(rx.telemetry(tel, tel.seq), "telemetry after the key import");
        CHECK(tel.events.size() == 3 && tel.events[0].model == "Princeton" && tel.events[0].id == "00 00 00 00 00 95 D5 D4", "%zu events, first %s %s", tel.events.size(),
              tel.events.empty() ? "" : tel.events[0].model.c_str(), tel.events.empty() ? "" : tel.events[0].id.c_str());
        CHECK(summary.find("Princeton") != std::string::npos && summary.find("24 bits") != std::string::npos, "summary '%s'", summary.c_str());
        printf("  import: %s\n", summary.c_str());
        // a bad file changes nothing
        CHECK(!rx.importSubFile("not a sub file", summary, err) && !err.empty(), "bad file accepted");
        CHECK(!rx.telemetry(tel, tel.seq), "telemetry changed by a bad file");
        // the lists stay when the receiver then starts (the feed thread takes them over)
        rx.configure(1e6);
        std::vector<cf32> z(1 << 14, cf32(0, 0));
        rx.feed(z.data(), z.size());
        rx.feed(z.data(), z.size());
        IsmTelemetry t2;
        rx.telemetry(t2, 0);
        CHECK(t2.devices.size() == 1 && t2.events.size() == 3 && t2.raws.size() == 1, "after start: %zu devices, %zu events, %zu raws", t2.devices.size(), t2.events.size(), t2.raws.size());
    }
    return testSummary("ism_subfile");
}
