// Ham operation (WSJT-X style) for FT8 / FT4 / FT2: the amateur band table, the TX gate, the ADIF log with worked-before, and the QSO
// auto-sequence engine. Pure logic: no audio, no radio, no clock of its own (the caller passes UTC seconds). Nothing here transmits;
// the app asks the engine for the message of a slot and sends it only through the TX path when the gate allows it.
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace dect2::ham {

// ---- amateur bands (union of the ITU regions; the operator's own licence class and national plan still apply) ----
struct HamBand { const char* name; double loHz, hiHz; };
const std::vector<HamBand>& hamBands();
const HamBand* hamBandAt(double hz);                         // the band containing hz, or nullptr
// A signal at dialHz + audioHz .. + bwHz must lie wholly inside one amateur band.
bool hamSignalInBand(double dialHz, double audioHz, double bwHz);

// The one gate every ham transmission passes. Needs: licence confirmed, an explicit Arm, the signal inside an amateur band, a callsign and a locator.
struct HamTxCheck {
    bool licenceConfirmed = false, armed = false;
    std::string call, grid;
    double dialHz = 0, audioHz = 0, bwHz = 50;
};
bool hamTxAllowed(const HamTxCheck& c, std::string& why);    // why: the reason when false (empty when true)
bool hamCallValid(const std::string& call);                  // plain callsign shape (prefix, digit, suffix), optional /P /M style suffix
bool hamGridValid(const std::string& grid);                  // 4 or 6 character Maidenhead locator

// ---- ADIF log ----
struct QsoRecord {
    std::string call, grid, mode = "FT8", rstSent, rstRcvd, myCall, myGrid;
    std::string submode;          // ADIF SUBMODE (mode "PSK", submode "PSK31"); "" none
    double freqHz = 0, startUtc = 0, endUtc = 0;
};
std::string bandNameFor(double hz);                          // "20m" ADIF band name, "" outside amateur bands
std::string adifRecord(const QsoRecord& q);                  // one record, ends with <EOR>
std::string adifHeader();
bool adifAppend(const std::string& path, const QsoRecord& q, std::string& err);   // creates the file with a header when new
std::vector<QsoRecord> adifParse(const std::string& text);   // call, grid, mode, frequency, rst, myCall: what the worked-before marks need

class WorkedLog {
public:
    void add(const QsoRecord& q);
    bool load(const std::string& path);
    bool worked(const std::string& call) const;                                  // ever, any band or mode
    bool workedOn(const std::string& call, const std::string& band, const std::string& mode) const;
    size_t size() const { return keys_.size(); }
private:
    std::vector<std::string> keys_;   // CALL|band|MODE
};

// ---- slot timing ----
// Mode numbers are those of hfdig_ftx.h: 0 FT8, 1 FT4, 2 FT2, 3 WSPR, 4 JT65, 5 JT9, 10 JT4.
double slotPeriod(int mode);                                 // 15 / 7.5 / 3.75 s, WSPR 120 s, JT65 / JT9 / JT4 60 s
const char* hamModeName(int mode);                           // "FT8" .. "JT4" (the ADIF MODE)
bool hamModeHasQso(int mode);                                // the modes the QSO engine runs: FT8 FT4 FT2 JT65 JT9 JT4
bool hamModeUsesRRR(int mode);                               // JT65 / JT9 / JT4 end with RRR, the FT modes with RR73
double hamModeBwHz(int mode, int sub);                       // occupied bandwidth for the TX gate (JT65 sub 0 A 1 B 2 C, JT4 sub 0 A .. 6 G)
long slotIndex(double utc, double period);
bool slotEven(double utc, double period);
double slotStart(long index, double period);
double txStartDelay(int mode);                               // s after the slot start where the transmission begins (WSJT-X: 0.5 FT8/FT4)

// ---- message builders (standard FT8 / FT4 / FT2 messages) ----
std::string msgCq(const std::string& me, const std::string& grid);
std::string msgGrid(const std::string& to, const std::string& me, const std::string& grid);
std::string msgReport(const std::string& to, const std::string& me, int snr, bool roger);
std::string msgRR73(const std::string& to, const std::string& me);
std::string msgRRR(const std::string& to, const std::string& me);
std::string msg73(const std::string& to, const std::string& me);
std::string msgFree(const std::string& text);                // upper case, 13 characters at most, only the characters FT8 free text allows
std::string reportText(int snr);                             // "-05" "+12", clamped to -30..+32

struct ParsedMsg {
    enum Kind { Other, Cq, Grid, Report, RReport, RR73, Rrr, Bye73 } kind = Other;
    std::string to, from, grid;
    int report = 0;
};
ParsedMsg parseMessage(const std::string& msg);

// ---- the QSO engine ----
struct QsoConfig {
    std::string myCall, myGrid;
    int mode = 0;                 // slot length via slotPeriod (hfdig mode number)
    bool txEven = true;           // transmit in even slots (a station answered is on the other parity)
    bool autoSeq = true;
    bool hold = false;            // Hold TX freq: a call to a station does not move the TX frequency to theirs
    double txHz = 1500;
    int retryLimit = 4;           // transmissions of one message without an answer before the call is given up
    double watchdogSec = 360;     // no user action for this long: halt
};

class QsoEngine {
public:
    enum State { Idle, Cq, Tx1Grid, Tx2Report, Tx3RReport, Tx4RR73, Tx5Bye };
    using Config = QsoConfig;
    explicit QsoEngine(const QsoConfig& c = QsoConfig()) : c_(c) {}
    Config& config() { return c_; }
    State state() const { return s_; }
    const std::string& dxCall() const { return dx_; }
    bool active() const { return s_ != Idle; }

    void startCq(double nowUtc);
    void callStation(double nowUtc, const std::string& call, const std::string& grid, int snr, double theirSlotUtc, double theirHz);
    void halt();                                               // Halt TX: back to idle, nothing is sent
    void userAction(double nowUtc) { lastUser_ = nowUtc; }     // resets the watchdog
    // A decoded message (any; the engine ignores what is not for it).
    void onDecode(double nowUtc, const std::string& msg, int snr, double slotUtc, double hz);
    // The message to send in the slot starting at slotUtc, or "" when nothing is due (idle, wrong parity, watchdog, retries used up).
    std::string messageForSlot(double slotUtc);
    // Called when the transmission of that slot has been started (counts the retry, finishes the 73 state).
    void txSent(double slotUtc);
    std::function<void(const QsoRecord&)> onLog;               // a QSO is complete
    std::function<void(const std::string&)> onEvent;           // "watchdog", "retries"
    bool watchdogHit() const { return wd_; }

private:
    void logQso(double nowUtc);
    void note(const std::string& s) { if (onEvent) onEvent(s); }
    Config c_;
    State s_ = Idle;
    std::string dx_, dxGrid_, rstRcvd_;
    int dxSnr_ = 0, sentCount_ = 0;
    double lastUser_ = 0, startUtc_ = 0, nowUtc_ = 0;
    bool resumeCq_ = false, wd_ = false, logged_ = false;
};

// ---- the WSPR beacon (transmit only): CALL GRID DBM in some of the 2 minute slots, never more than half of them ----
struct WsprBeaconConfig {
    std::string myCall, myGrid;
    int dbm = 10;                 // 0 3 7 10 13 .. 60 (the values WSPR can carry)
    int everyNth = 5;             // one 2 minute slot in N, N >= 2 (duty cycle 1/N, never above 50 %); default 20 %
    int offset = 0;               // which of the N (a station of two beacons uses different ones)
    double audioHz = 1500;        // inside the 200 Hz WSPR window, 1400 .. 1600
    int maxTx = 30;               // transmissions without any operator action, then the beacon stops
};
bool wsprDbmValid(int dbm);
class WsprBeacon {
public:
    WsprBeaconConfig cfg;
    void start() { on_ = true; sent_ = 0; stopped_.clear(); }
    void stop() { on_ = false; }
    bool active() const { return on_; }
    void userAction() { sent_ = 0; }                               // the operator touched the controls: the maxTx count starts again
    const std::string& stopReason() const { return stopped_; }
    std::string message() const;                                    // "LY1ABC KO24 10", "" when call / grid / power are not valid
    // The message to send in the slot starting at slotUtc, or "" (not running, not a due 2 minute slot, count used up, bad config).
    std::string messageForSlot(double slotUtc);
    void txSent() { sent_++; }
    double audioHz() const;                                         // cfg.audioHz clamped to 1400 .. 1600
private:
    bool on_ = false;
    int sent_ = 0;
    std::string stopped_;
};

// ---- the slot transmitter: one FT8 / FT4 / FT2 burst through a rig (PTT + audio) ----
struct HamRigIo {   // the app's real one is Hamlib PTT + the sound card; tests use a fake
    virtual ~HamRigIo() = default;
    virtual bool ptt(bool on, std::string& err) = 0;
    virtual bool beginAudio(int rate, std::string& err) = 0;
    virtual int write(const float* mono, int frames) = 0;   // frames accepted (may be fewer than offered)
    virtual int pending() = 0;                              // frames queued and not yet played
    virtual void flushAudio() = 0;                          // drop what is queued
    virtual void endAudio() = 0;
};
class HamSlotTx {
public:
    explicit HamSlotTx(HamRigIo& io) : io_(io) {}
    // Blocking (run it on a worker thread): gate, modulate, PTT on, play, PTT off. True only when the whole burst was played.
    // PTT is released on every exit path. Nothing is keyed when the gate refuses.
    // mode: FT8 / FT4 / FT2 / WSPR / JT65 / JT9 / JT4 (hfdig numbers); sub: JT65 A B C, JT4 A .. G. The gate's bandwidth is raised to the mode's own.
    bool send(const HamTxCheck& gate, int mode, const std::string& msg, double audioHz, double level, std::string& err, int sub = 0);
    // Any other audio (the keyboard modes): the same gate, PTT, drain and release as send(). audio is mono at 48000 Hz; bwHz raises the gate's width;
    // it is scaled to peak `level` first.
    bool sendAudio(const HamTxCheck& gate, std::vector<float> audio, double bwHz, double level, std::string& err);
    void abort();        // Halt TX, any thread: audio dropped, PTT released at once
    bool busy() const;
private:
    bool pttSet(bool on, std::string& err);
    bool play(const std::vector<float>& a, size_t n, int rate, std::string& err);   // after the gate: PTT on, audio, drain, PTT off
    HamRigIo& io_;
    std::mutex mu_;
    std::atomic<bool> abort_{false}, busy_{false}, pttOn_{false};
};

}  // namespace dect2::ham
