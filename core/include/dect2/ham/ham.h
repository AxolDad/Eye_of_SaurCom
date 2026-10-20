// Ham operation (WSJT-X style) for FT8 / FT4 / FT2: the amateur band table, the TX gate, the ADIF log with worked-before, and the QSO
// auto-sequence engine. Pure logic: no audio, no radio, no clock of its own (the caller passes UTC seconds). Nothing here transmits;
// the app asks the engine for the message of a slot and sends it only through the TX path when the gate allows it.
#pragma once
#include <functional>
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
double slotPeriod(int mode);                                 // 0 FT8 15 s, 1 FT4 7.5 s, 2 FT2 3.75 s (the hfdig mode numbers)
long slotIndex(double utc, double period);
bool slotEven(double utc, double period);
double slotStart(long index, double period);
double txStartDelay(int mode);                               // s after the slot start where the transmission begins (WSJT-X: 0.5 FT8/FT4)

// ---- message builders (standard FT8 / FT4 / FT2 messages) ----
std::string msgCq(const std::string& me, const std::string& grid);
std::string msgGrid(const std::string& to, const std::string& me, const std::string& grid);
std::string msgReport(const std::string& to, const std::string& me, int snr, bool roger);
std::string msgRR73(const std::string& to, const std::string& me);
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
    int mode = 0;                 // slot length via slotPeriod
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

}  // namespace dect2::ham
