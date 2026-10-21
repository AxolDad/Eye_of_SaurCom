// Analog TV receiver: the glue between the radio side (atv_front), the picture side (atv_video), the sound and the outside world.
//
//   input -> DC removal -> [searching: spectrum -> vision carrier + sound carrier]
//                       -> [running: vision channel -> picture decoder -> pictures]
//                                    sound channel  -> 48 kHz mono -> sound device
#include "dect2/atv/atv_rx.h"
#include "atv_front.h"
#include "atv_fm.h"
#include "atv_video.h"
#include "dect2/atv/atv_nbtv.h"
#include "dect2/audio/audioout.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace dect2 {

struct AtvReceiver::Impl {
    double fs = 0;
    bool ready = false;
    std::mutex mu;
    AtvTelemetry tel;
    uint64_t telSeq = 0;
    std::shared_ptr<const AtvFrame> latest;
    std::function<void(const std::string&)> logCb;

    AtvCarrierSearch search;
    AtvCarrierSearch searchInv;         // the same on the mirrored spectrum (I and Q swapped by the radio or the file format)
    bool inverted = false;              // the carrier in use was found in the mirrored spectrum: the input is conjugated
    std::vector<cf32> xc;
    std::vector<cf32> clean;            // the input with non-finite samples replaced
    AtvFront front;
    AtvVideo video;
    AtvSound sound;
    AtvFmDemod fm;                      // FM video (FPV): the frequency of the carrier is the picture
    std::atomic<int> modulation{0};     // 0 AM, 1 FM, 2 NBTV on AM, 3 NBTV on FM, as asked for (setModulation)
    // NBTV and mechanical television: the video is the audio of an AM or narrow FM signal at the centre of the input
    AtvNbtvRadio nbtv;
    std::mutex nbtvMu;
    NbtvFormat nbtvAsk;                 // what setNbtv asked for (under nbtvMu)
    double nbtvDevAsk = 5000;
    std::atomic<bool> nbtvDirty{false};
    NbtvFormat nbtvFmt;
    double nbtvDev = 5000;
    int active = 0;                     // the one the receiver is set up for
    bool fmSyncLow = true;
    uint64_t fmFlipAt = 0;


    // state
    enum Mode { kSearch, kTrial, kRun } mode = kSearch;
    uint64_t nIn = 0;                       // input samples so far
    uint64_t trialAt = 0, lostAt = 0, lastTelAt = 0;
    AtvCarrier carrier;                     // the one being followed
    bool haveCarrier = false;
    std::vector<AtvCarrier> pending;        // the other candidates of the last search, best first
    struct Bad { double hz; uint64_t until; };
    std::vector<Bad> bad;
    cf32 dc = cf32(0, 0);
    float dcA = 2e-4f;
    std::vector<cf32> xd;
    std::vector<float> audioBuf;
    double soundOff = 0;                     // sound carrier spacing in Hz (from the vision carrier)
    int sysHint = 0;

    // output
    std::unique_ptr<AudioOut> audio;
    std::atomic<bool> silent{false}, muted{false};
    std::atomic<float> volume{1.f};
    std::function<void(const float*, const float*, size_t)> tap;
    std::mutex tapMu;
    std::vector<float> inter;

    // controls
    AtvVideoParams prm;
    std::atomic<int> detMode{0};
    std::atomic<int> chanWidthMhz{8};
    std::atomic<bool> prmDirty{false};
    std::mutex prmMu;
    AtvVideoParams prmNew;
    bool syncOn = false;

    void log(const std::string& s) { if (logCb) logCb(s); }

    void configure(double rate) {
        fs = rate;
        ready = false;
        if (rate < 7.9e6) return;
        dcA = (float)(1.0 / (0.0005 * rate));
        search.configure(rate);
        searchInv.configure(rate);
        front.configure(rate);
        sound.configure(rate);
        video.configure(front.videoRate(), front.colourCapable());
        videoRateNow = front.videoRate();
        video.setNoiseScale(front.noiseGain() * rate / 5e6);
        video.carrierError = [this](double rad, double trust) { front.carrierError(rad, trust); };
        video.log = [this](const std::string& s) { log(s); };
        video.frameReady = [this](std::shared_ptr<const AtvFrame> f) { std::lock_guard<std::mutex> lk(mu); latest = std::move(f); };
        ready = true;
        active = 0;
        resetAll();
        if (modulation.load() == 1) switchModulation(1);
        if (!silent.load() && !audio) { /* opened on the first audio */ }
    }

    // broadcast transmitters send the colour 170 ns early (BT.470), which the decoder puts right; an FPV transmitter does not
    void applyPrm() {
        AtvVideoParams p = prm;
        if (active == 1 && p.chromaDelayNs < 0) p.chromaDelayNs = 0;
        video.setParams(p);
    }

    // AM and FM share the picture decoder; it takes the video rate of the radio side in use
    void startNbtv() {
        {
            std::lock_guard<std::mutex> lk(nbtvMu);
            nbtvFmt = nbtvAsk; nbtvDev = nbtvDevAsk;
        }
        nbtv.configure(fs, nbtvFmt, active == 3, nbtvDev);
        nbtv.decoder().frameReady = [this](std::shared_ptr<const AtvFrame> f) { std::lock_guard<std::mutex> lk(mu); latest = std::move(f); };
        nbtv.decoder().log = [this](const std::string& s) { log(s); };
        char b[160];
        snprintf(b, sizeof b, "analog TV: %s from %s, audio at %.1f kHz", nbtvFmt.name().c_str(), active == 3 ? "narrow FM" : "AM", nbtv.audioRate() / 1e3);
        log(b);
    }

    void switchModulation(int m) {
        if (!ready) return;
        active = m;
        if (m >= 2) startNbtv();
        else if (m == 1) {
            if (fs < 15.9e6) log("analog TV, FM video: a link is about 17 MHz wide, this input rate is too low (use 20 Msps)");
            fm.configure(fs);
            video.configure(fm.videoRate(), fm.videoRate() >= 9.5e6);
            video.setNoiseScale(1.0);
            video.setFmVideo(true);
            applyPrm();
        } else {
            video.configure(front.videoRate(), front.colourCapable());
            videoRateNow = front.videoRate();
            video.setNoiseScale(front.noiseGain() * fs / 5e6);
            video.setFmVideo(false);
            applyPrm();
        }
        resetAll();
    }

    // System E needs the full input rate for its 10 MHz of video: the vision channel's output rate follows the plan, the decoder follows it
    double videoRateNow = 0;
    void syncVideoRate() {
        if (active != 0 || front.videoRate() == videoRateNow) return;
        videoRateNow = front.videoRate();
        video.configure(front.videoRate(), front.colourCapable());
        video.setFmVideo(false);
        applyPrm();
    }

    // the vestigial sideband that goes with a sound spacing: 1.25 MHz with 6.0 MHz sound (I), 2 MHz with 11.15 (E), 0.75 otherwise
    static double vsbFor(double spacing) { return spacing > 5.8 && spacing < 6.1 ? 1.25 : spacing > 8 ? 2.0 : 0; }

    void resetAll() {
        if (!ready) return;
        search.setMonitor(false);
        searchInv.reset(); inverted = false;
        front.reset();
        front.setPlan(5.5);
        syncVideoRate();
        video.setNoiseScale(front.noiseGain() * fs / 5e6);
        pending.clear();
        video.reset();
        sound.reset();
        mode = kSearch; haveCarrier = false; syncOn = false;
        front.setSyncDetector(false);
        dc = cf32(0, 0);
        trialAt = lostAt = 0;
        bad.clear();
        if (audio) audio->flush();
        {
            std::lock_guard<std::mutex> lk(mu);
            tel = AtvTelemetry();
            tel.seq = ++telSeq;
            latest.reset();
        }
        if (active == 1) startFm();
        if (active >= 2) { nbtv.reset(); search.setMonitor(true); mode = kRun; }
    }

    void startFm() {
        fm.reset();
        fmSyncLow = true; fm.setSyncLow(true);
        search.setMonitor(true);          // the spectrum for the display
        video.setSoundSpacing(0);
        mode = kTrial; trialAt = nIn; lostAt = 0; fmFlipAt = nIn;
    }

    static bool isAmSpacing(double s) { return (s > 3.4 && s < 3.6) || s > 8; }

    // The video band follows the sound spacing, except for SECAM chosen on M or N: its subcarriers reach 4.756 MHz, above the 4.5 MHz of
    // their sound carrier (which such a signal cannot have): the band of the 5.5 MHz plan then
    double planSpacing(double spacing) const {
        const bool secamNarrow = prm.forceColour == kAtvSecam && (prm.forceSys == kAtvM || prm.forceSys == kAtvN || (prm.forceSys < 0 && spacing > 0 && spacing < 5));
        if (secamNarrow) return 5.5;
        return spacing > 0 ? spacing : 5.5;
    }

    bool isBad(double hz) const {
        for (const auto& b : bad) if (b.until > nIn && std::fabs(hz - b.hz) < 60e3) return true;
        return false;
    }

    bool trialWeak = false;
    uint64_t afcAt = 0;
    double afcPrev = 0;
    bool afcHave = false;
    int goneN = 0;
    bool haveRef = false;
    double refDb = 0;

    // The carrier follows the strongest line in the spectrum near it: the loop on the sync tips holds it to within a few kilohertz, this brings it
    // back from a jump or a drift that is faster or larger than that. Two checks 0.2 s apart have to agree before it moves.
    void afc() {
        if (nIn - afcAt < (uint64_t)(0.2 * fs) || search.spectra() < 6) return;
        afcAt = nIn;
        double f, prom, lvl;
        const double c = front.carrierHz();
        if (!search.peakNear(c, 150e3, f, prom, lvl)) return;
        // the line is as strong as it was when the carrier was taken, within 12 dB; if not, the signal went somewhere else, or went off
        if (!haveRef) { refDb = lvl; haveRef = true; }
        refDb = std::max(refDb - 0.1, lvl);
        if (prom < 12 || lvl < refDb - 12) {
            afcHave = false;
            if (++goneN >= 4) { goneN = 0; log("analog TV: the vision carrier is gone"); backToSearch(); pending.clear(); }
            return;
        }
        goneN = 0;
        if (prom < 20) { afcHave = false; return; }
        const double d = f - c;
        if (std::fabs(d) < 1.5e3) { afcHave = false; return; }
        if (afcHave && std::fabs(d - afcPrev) < 1.0e3) {
            char b[120];
            snprintf(b, sizeof b, "analog TV: vision carrier moved by %+.1f kHz", d / 1e3);
            log(b);
            front.setCarrier(f);
            afcHave = false;
        } else { afcHave = true; afcPrev = d; }
    }
    void startCarrier(const AtvCarrier& c, bool weak = false) {
        carrier = c; haveCarrier = true; trialWeak = weak;
        front.setPlan(planSpacing(c.spacingMhz), vsbFor(c.spacingMhz));
        syncVideoRate();
        video.setSlicerWidth(weak ? 2.4 : 1.0);
        video.setNoiseScale(front.noiseGain() * fs / 5e6);
        search.setMonitor(true);
        front.setCarrier(c.visionHz);
        front.setDsbAmount((float)std::min(1.0, std::max(0.0, (c.dsbDb + 8.0) / 8.0)));
        front.setSyncDetector(false); syncOn = false;
        video.syncDetectorChanged(false);
        video.reset();
        video.setSoundSpacing(c.spacingMhz);
        video.setMirrored(inverted);
        soundOff = c.spacingMhz > 0 ? c.spacingMhz * 1e6 : 5.5e6;
        sound.reset();
        sound.setSystem(c.spacingMhz > 0 && c.spacingMhz < 5 ? 25 : 50, c.spacingMhz > 0 && c.spacingMhz < 5 ? 75 : 50);
        // AM sound with 3.5 MHz (A) and 11.15 MHz (E) spacing; with the others once the picture says the modulation is positive (C, L)
        sound.setAm(isAmSpacing(c.spacingMhz));
        mode = kTrial; trialAt = nIn; lostAt = 0; goneN = 0; afcHave = false; haveRef = false;
        char b[200];
        snprintf(b, sizeof b, "analog TV: vision carrier at %+.3f MHz (%.0f dB above its surroundings)%s", c.visionHz / 1e6, c.visionDb, c.spacingMhz > 0 ? ", sound carrier found" : "");
        log(b);
    }

    void feed(const cf32* x, size_t n) {
        if (!ready) return;
        {
            std::lock_guard<std::mutex> lk(prmMu);
            if (prmDirty.exchange(false)) {
                prm = prmNew; applyPrm();
                if (haveCarrier && active == 0) { front.setPlan(planSpacing(carrier.spacingMhz), vsbFor(carrier.spacingMhz)); syncVideoRate(); }
            }
        }
        if (modulation.load() != active) switchModulation(modulation.load());
        else if (active >= 2 && nbtvDirty.exchange(false)) { startNbtv(); resetAll(); }
        // a NaN or infinite sample (a broken file) would stay for good in the DC estimate and the loops: such samples count as silence
        for (size_t k = 0; k < n; k++) {
            if (std::isfinite(x[k].real() + x[k].imag())) continue;
            clean.assign(x, x + n);
            for (cf32& v : clean) if (!std::isfinite(v.real() + v.imag())) v = cf32(0, 0);
            x = clean.data();
            break;
        }
        size_t i = 0;
        while (i < n) {
            const size_t m = std::min(n - i, AtvFront::kMaxBlock);
            block(x + i, m);
            i += m;
        }
    }

    // the next carrier of the last search that has not failed a trial
    bool nextCandidate() {
        while (!pending.empty()) {
            const AtvCarrier c = pending.front();
            pending.erase(pending.begin());
            if (isBad(c.visionHz) || c.visionDb < 12) continue;
            startCarrier(c);
            return true;
        }
        return false;
    }

    void backToSearch() {
        mode = kSearch; haveCarrier = false; search.setMonitor(false); sound.reset();
        front.setSyncDetector(false); syncOn = false; video.syncDetectorChanged(false);
    }

    // the radio's centre spike out of the block: the mean of the block moves a slow estimate, once per block. The picture and the sound carrier
    // turn at megahertz, so their mean over a block is nothing; the spike does not turn.
    void removeDc(const cf32* x, size_t m) {
        xd.resize(m);
        // the sums in four lanes: a single running sum would make every addition wait for the last
        float sr4[4] = {0, 0, 0, 0}, si4[4] = {0, 0, 0, 0};
        size_t k = 0;
        for (; k + 4 <= m; k += 4) for (size_t l = 0; l < 4; l++) { sr4[l] += x[k + l].real(); si4[l] += x[k + l].imag(); }
        for (; k < m; k++) { sr4[0] += x[k].real(); si4[0] += x[k].imag(); }
        const float sr = (sr4[0] + sr4[1]) + (sr4[2] + sr4[3]), si = (si4[0] + si4[1]) + (si4[2] + si4[3]);
        const float w = std::min(1.f, dcA * (float)m);
        dc += (cf32(sr, si) / (float)m - dc) * w;
        const float dr = dc.real(), di = dc.imag();
        for (size_t q = 0; q < m; q++) xd[q] = cf32(x[q].real() - dr, x[q].imag() - di);
    }

    // FM video: no carrier to find; the discriminator is the detector. The polarity (which end of the swing is the sync tip) is tried: when no lines
    // lock within a second and a bit, the other one is.
    void blockFm(const cf32* x, size_t m) {
        removeDc(x, m);
        nIn += m;
        search.feed(xd.data(), m);
        const size_t nv = fm.process(xd.data(), m);
        if (nv) {
            // the colour is demodulated from the i and q of the radio side: here the analytic signal of the video
            video.process(fm.v(), fm.i(), fm.q(), nv);
        }
        if (mode == kTrial) {
            if (video.lineLocked()) {
                mode = kRun; lostAt = 0;
                log(std::string("analog TV, FM video: locked, the sync tip is the ") + (fmSyncLow ? "lowest" : "highest") + " frequency");
            } else if (nIn - fmFlipAt > (uint64_t)(1.2 * fs)) {
                fmSyncLow = !fmSyncLow; fm.setSyncLow(fmSyncLow);
                video.reset();
                fmFlipAt = nIn;
                log(std::string("analog TV, FM video: no lines, trying the sync tip at the ") + (fmSyncLow ? "lowest" : "highest") + " frequency");
            }
        } else if (mode == kRun) {
            if (video.lineLocked()) lostAt = 0;
            else { if (!lostAt) lostAt = nIn; if (nIn - lostAt > (uint64_t)(2.0 * fs)) { log("analog TV, FM video: signal lost"); mode = kTrial; fmFlipAt = nIn; } }
        }
        if (nIn - lastTelAt >= (uint64_t)(fs * 0.25)) { lastTelAt = nIn; publish(); }
    }

    // NBTV: demodulate the carrier at the centre, the picture decoder works on the audio
    void blockNbtv(const cf32* x, size_t m) {
        // no DC removal: the carrier sits at the centre, where the radio's own DC is (a carrier stronger than that spike is needed)
        nIn += m;
        search.feed(x, m);
        nbtv.process(x, m);
        if (nIn - lastTelAt >= (uint64_t)(fs * 0.25)) { lastTelAt = nIn; publish(); }
    }

    void block(const cf32* x, size_t m) {
        if (active >= 2) { blockNbtv(x, m); return; }
        if (active == 1) { blockFm(x, m); return; }
        removeDc(x, m);
        nIn += m;
        if (mode == kSearch) {
            search.feed(xd.data(), m);
            xc.resize(m);
            for (size_t q = 0; q < m; q++) xc[q] = std::conj(xd[q]);
            searchInv.feed(xc.data(), m);
            if (search.spectra() >= 24) {
                // the mirrored spectrum wins only clearly (its sound carrier sits on the right side of the vision carrier, its vestigial sideband
                // has the right shape), so a normal signal never turns
                std::vector<AtvCarrier> a = search.candidates(), b = searchInv.candidates();
                const double sa = a.empty() ? -1e9 : a[0].score, sb = b.empty() ? -1e9 : b[0].score;
                const bool inv = sb > sa + 5;
                if (inv != inverted) log(inv ? "analog TV: the spectrum is mirrored (I and Q swapped)" : "analog TV: the spectrum is the right way round");
                inverted = inv;
                pending = inv ? b : a;
                searchInv.reset();
                if (!nextCandidate()) search.reset();
            }
        } else {
            if (inverted) for (size_t q = 0; q < m; q++) xd[q] = std::conj(xd[q]);
            search.feed(xd.data(), m);          // the spectrum for the display, a few FFTs a second
            runBlock(m);
        }
        if (nIn - lastTelAt >= (uint64_t)(fs * 0.25)) { lastTelAt = nIn; publish(); }
    }

    void runBlock(size_t m) {
        afc();
        const size_t nv = front.process(xd.data(), m);
        if (nv) video.process(front.v(), front.i(), front.q(), nv);
        // the detector: synchronous once the carrier loop is locked (and the user did not ask for the envelope)
        const bool want = detMode.load() == 0 && video.wantSyncDetector();
        if (want != syncOn) { syncOn = want; front.setSyncDetector(want); video.syncDetectorChanged(want); }
        // sound: AM with the positive-modulation systems
        {
            const bool am = isAmSpacing(carrier.spacingMhz) || (video.lineLocked() && video.positive());
            if (am != sound.am()) sound.setAm(am);
        }
        sound.setCarrier(front.carrierHz() + soundOff);
        audioBuf.clear();
        sound.process(xd.data(), m, audioBuf);
        emitAudio();
        // supervision
        if (mode == kTrial) {
            if (video.lineLocked()) { mode = kRun; lostAt = 0; log("analog TV: locked"); }
            else if (nIn - trialAt > (uint64_t)((trialWeak ? 1.6 : 1.0) * fs)) {
                if (!trialWeak) {                                    // the lines may be under the noise: the same carrier again with a wider slicer
                    log("analog TV: no lines found, trying again for a weak signal");
                    startCarrier(carrier, true);
                } else {
                    bad.push_back({carrier.visionHz, nIn + (uint64_t)(6 * fs)});
                    log("analog TV: no picture signal on that carrier");
                    if (!nextCandidate()) backToSearch();
                }
            }
        } else if (mode == kRun) {
            if (video.lineLocked()) lostAt = 0;
            else { if (!lostAt) lostAt = nIn; if (nIn - lostAt > (uint64_t)(2.0 * fs)) { log("analog TV: signal lost"); backToSearch(); pending.clear(); } }
        }
    }

    void emitAudio() {
        if (audioBuf.empty()) return;
        const size_t n = audioBuf.size();
        const float vol = volume.load();
        {
            std::lock_guard<std::mutex> lk(tapMu);
            if (tap) tap(audioBuf.data(), audioBuf.data(), n);
        }
        if (silent.load()) return;
        if (!audio) {
            audio = std::make_unique<AudioOut>();
            audio->start(48000);
            audio->setStartThreshold(48000 / 5);
        }
        audio->setVolume(vol); audio->setMuted(muted.load());
        inter.resize(n * 2 + 2);
        for (size_t i = 0; i < n; i++) { inter[2 * i] = audioBuf[i]; inter[2 * i + 1] = audioBuf[i]; }
        int frames = (int)n;
        if (audio->playing()) {
            // the radio's clock and the sound card's differ by a few ppm: keep the queue near 0.2 s by adding or removing one frame per block
            const int queued = audio->bufferedFrames();
            if (queued > 14400 && frames > 1) frames--;
            else if (queued < 4800 && frames > 0) { inter[2 * (size_t)frames] = inter[2 * (size_t)frames - 2]; inter[2 * (size_t)frames + 1] = inter[2 * (size_t)frames - 1]; frames++; }
        }
        audio->write(inter.data(), frames);
    }

    void publishNbtv() {
        AtvTelemetry t;
        const NbtvStatus s = nbtv.decoder().status();
        t.system = nbtvFmt.name(); t.colourSystem = "mono"; t.lines = nbtvFmt.lines;
        t.lineHz = s.lineHz; t.lineErrPpm = s.lineErrPpm; t.fieldHz = s.lineHz / nbtvFmt.lines;
        t.syncQuality = s.syncQuality; t.lineCount = s.lines; t.frameCount = s.frames; t.fieldCount = s.frames;
        t.snrDb = s.snrDb; t.lineWave = s.lineWave;
        t.carrierDbfs = nbtv.carrierDbfs();
        t.state = s.frameLocked ? 2 : s.lineLocked ? 1 : 0;
        t.dataValid = t.state == 2;
        t.blocksOk = s.frames;
        t.modulation = active;
        t.specLoMhz = -fs / 2e6 * 0.9; t.specHiMhz = fs / 2e6 * 0.9;
        search.display(0, t.specLoMhz, t.specHiMhz, 170, t.specDb);
        std::lock_guard<std::mutex> lk(mu);
        t.seq = ++telSeq;
        tel = std::move(t);
    }

    void publish() {
        if (active >= 2) { publishNbtv(); return; }
        AtvTelemetry t;
        const AtvVideo::Info vi = video.info();
        t.system = vi.system; t.colourSystem = vi.colourSystem; t.lines = vi.lines;
        t.fieldHz = vi.fieldHz;
        t.colour = vi.colour; t.colourKiller = vi.killer;
        t.syncDetector = syncOn;
        t.snrDb = vi.snrDb;
        t.lineHz = vi.lineHz; t.lineErrPpm = vi.lineErrPpm;
        t.syncQuality = vi.syncQuality;
        t.lineCount = vi.lines_; t.fieldCount = vi.fields; t.frameCount = vi.frames; t.fieldNo = vi.fieldNo;
        t.syncDepthPct = vi.syncTip; t.whitePeak = vi.whitePeak; t.burstLevel = vi.burstLevel; t.syncCompressionPct = vi.syncCompressionPct; t.chromaPhaseErrDeg = vi.chromaErrDeg;
        t.blocksOk = vi.fieldsOk; t.blocksBad = vi.fieldsBad;
        t.lineWave = vi.lineWave; t.vbiWave = vi.vbiWave;
        t.soundPresent = sound.present(); t.soundDevKhz = sound.devKhz(); t.soundLevelDb = sound.levelDb();
        if (haveCarrier) {
            const AtvFormat vf = video.format();
            // the systems added with positive modulation (A, C, E, L, L') and H, K': the channel layout of the system that was found
            const bool newSys = !vi.system.empty() && vf.sys >= kAtvA;
            // a channel that is turned round by nature (A, L') is read in the mirrored spectrum: the carriers are reported where they are
            const bool turned = newSys && vf.mirrored && inverted;
            t.visionHz = turned ? -front.carrierHz() : front.carrierHz();
            t.soundSpacingMhz = turned ? -carrier.spacingMhz : carrier.spacingMhz;
            t.soundHz = carrier.spacingMhz > 0 ? (turned ? -(front.carrierHz() + carrier.spacingMhz * 1e6 + sound.trimHz()) : t.visionHz + carrier.spacingMhz * 1e6 + sound.trimHz()) : 0;
            t.carrierDbfs = vi.carrierLevel > 0 ? (float)(20 * std::log10(vi.carrierLevel)) : -120.f;
            t.carrierToNoiseDb = vi.carrierToNoiseDb;
            // The offset from where the standard puts the carrier in the channel (vision carrier 1.25 MHz above the channel's lower edge: -2.75,
            // -2.25 and -1.75 MHz from the centre of an 8, 7 and 6 MHz channel). The signal cannot say which width the channel has when the
            // sound is at +5.5 MHz (PAL B is 7 MHz, PAL G is 8 MHz, and an 8 MHz channel with the carrier 500 kHz high looks like a 7 MHz one),
            // so that case follows setChannelWidth() and assumes 8 MHz (the tuning table's width) when it is not set. 6.0 and 6.5 MHz
            // only exist in 8 MHz channels, 4.5 MHz and the 525-line systems only in 6 MHz channels.
            static const double kLay[3] = {-2.75e6, -2.25e6, -1.75e6};
            double best = kLay[0];
            if (vi.lines == 525 || carrier.spacingMhz == 4.5) best = kLay[2];
            else if (carrier.spacingMhz == 5.5 || carrier.spacingMhz == 0) {
                const int w = chanWidthMhz.load();
                best = w == 7 ? kLay[1] : w == 6 ? kLay[2] : kLay[0];
            }
            if (newSys) best = atvVisionOffsetHz(vf);
            t.cfoHz = t.visionHz - best;
            search.display(front.carrierHz(), t.specLoMhz, t.specHiMhz, 170, t.specDb);
        }
        t.modulation = active; t.fmSyncLow = fmSyncLow;
        if (active == 1) {   // no carrier: the spectrum of the whole input
            t.specLoMhz = -fs / 2e6 * 0.9; t.specHiMhz = fs / 2e6 * 0.9;
            search.display(0, t.specLoMhz, t.specHiMhz, 170, t.specDb);
        }
        t.state = ((haveCarrier || active == 1) && mode != kSearch) ? (video.fieldLocked() ? 2 : video.lineLocked() ? 1 : 0) : 0;
        t.dataValid = t.state == 2 || (sound.present() && mode == kRun);
        std::lock_guard<std::mutex> lk(mu);
        t.seq = ++telSeq;
        tel = std::move(t);
    }

    void setPrm(const AtvVideoParams& p) { std::lock_guard<std::mutex> lk(prmMu); prmNew = p; prmDirty = true; }
    AtvVideoParams curPrm() { std::lock_guard<std::mutex> lk(prmMu); return prmNew; }
};

AtvReceiver::AtvReceiver() : p_(std::make_unique<Impl>()) {}
AtvReceiver::~AtvReceiver() = default;

void AtvReceiver::configure(double inputRateHz) { p_->configure(inputRateHz); }
bool AtvReceiver::ready() const { return p_->ready && p_->fs >= atvTuning().minSampleRate - 1; }
void AtvReceiver::reset() { p_->resetAll(); }
void AtvReceiver::feed(const cf32* x, size_t n) { p_->feed(x, n); }
bool AtvReceiver::telemetry(AtvTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}
std::shared_ptr<const AtvFrame> AtvReceiver::frame(uint64_t& lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->latest || p_->latest->seq <= lastSeq) return nullptr;
    lastSeq = p_->latest->seq;
    return p_->latest;
}
void AtvReceiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->logCb = std::move(cb); }
void AtvReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void AtvReceiver::setMuted(bool m) { p_->muted = m; }
void AtvReceiver::setSilent(bool s) { p_->silent = s; }
void AtvReceiver::setAudioTap(std::function<void(const float*, const float*, size_t)> cb) { std::lock_guard<std::mutex> lk(p_->tapMu); p_->tap = std::move(cb); }
void AtvReceiver::setDeinterlace(int mode) { auto p = p_->curPrm(); p.deinterlace = mode; p_->setPrm(p); }
void AtvReceiver::setNoiseReduction(int mode) { auto p = p_->curPrm(); p.denoise = mode; p_->setPrm(p); }
void AtvReceiver::setStandard(int system, int colour) { auto p = p_->curPrm(); p.forceSys = system; p.forceColour = colour; p_->setPrm(p); }
void AtvReceiver::setBlackSetup(int mode) { auto p = p_->curPrm(); p.setupMode = mode; p_->setPrm(p); }
void AtvReceiver::setChromaDelayNs(double ns) { auto p = p_->curPrm(); p.chromaDelayNs = ns; p_->setPrm(p); }
void AtvReceiver::setColour(bool on) { auto p = p_->curPrm(); p.colourOn = on; p_->setPrm(p); }
void AtvReceiver::setSaturation(float s) { auto p = p_->curPrm(); p.saturation = s; p_->setPrm(p); }
void AtvReceiver::setHue(float d) { auto p = p_->curPrm(); p.hueDeg = d; p_->setPrm(p); }
void AtvReceiver::setDetector(int mode) { p_->detMode = mode; }
void AtvReceiver::setModulation(int mode) { p_->modulation = mode >= 0 && mode <= 3 ? mode : 0; }
void AtvReceiver::setNbtv(int lines, double frameHz, double devKhz, bool vertical) {
    NbtvFormat f = lines == 32 && frameHz == 12.5 ? nbtvStandard() : nbtvMechanical(lines, frameHz);
    f.vertical = vertical;
    {
        std::lock_guard<std::mutex> lk(p_->nbtvMu);
        p_->nbtvAsk = f; p_->nbtvDevAsk = devKhz > 0 ? devKhz * 1e3 : 5000;
    }
    p_->nbtvDirty = true;
}
void AtvReceiver::setChannelWidth(int mhz) { p_->chanWidthMhz = mhz; }

ModeTuning atvTuning() {
    ModeTuning t;
    t.stdMode = 10; t.id = "atv"; t.name = "Analog TV";
    t.minMhz = 45; t.maxMhz = 6000; t.defMhz = 600.0;
    t.sampleRate = 10000000.0; t.basebandHz = 9000000.0; t.bandwidthMhz = 8;
    t.minSampleRate = 8000000.0;
    // in system B/G the sound carrier (+2.75 MHz) is the mirror image of the vision carrier (-2.75 MHz) around the channel centre: the pair is
    // not circular, the blind IQ estimate reads it as an imbalance and its correction put the vision carrier's image on the sound (S/N 53 -> 40 dB)
    t.notCircular = true;
    return t;
}

} // namespace dect2
