// Trunking follower for P25 Phase 1 and 2 (TIA-102), and, as far as the public specs allow, DMR Tier III (ETSI TS 102 361-4), NXDN Type C,
// EDACS, LTR and MPT 1327. Receive only.
//
// Pieces, all plain logic with no DSP and no threads of their own (the interface thread drives them):
//   SystemModel     what the control channel says: site (WACN, system, RFSS, site, NAC), the channel identifier table, adjacent sites, the
//                   control-channel candidates, and the voice grants (as Grant records) made of TSBKs.
//   TalkgroupList   names, tags, priority, lockout and scan lists; CSV import / export (the RadioReference-style columns, see docs/trunking.md).
//   Follower        the state machine: grant -> follow (a slice of the wide capture, or a retune of the one radio) -> end -> back to the control channel.
//   CallLog         every call heard on the control channel (followed or not), a live "now talking" view, CSV export.
//   Controller      the four together; the host executes the Actions it returns and reports what the radios hear.
// Encrypted calls (P25 service options bit 0x40 or ALGID != 0x80, DMR privacy flag) are logged as encrypted and never followed, decoded or recorded.
// Voice audio only exists when the user-installed vocoder add-on is present (docs/CODECS.md); IDs, talkgroups and the call log work without it.
#pragma once
#include "dect2/p25/p25_proto.h"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace trunk {

// ------------------------------------------------------------------------------------------------ system model

// The system a call belongs to
enum class SysKind { P25 = 0, Dmr = 1, Nxdn = 2, Edacs = 3, Ltr = 4, Mpt1327 = 5 };
const char* sysKindName(SysKind k);
inline bool analogKind(SysKind k) { return k == SysKind::Edacs || k == SysKind::Ltr || k == SysKind::Mpt1327; }

struct Grant {
    SysKind kind = SysKind::P25;
    bool dmr = false;
    bool group = true;
    int tg = -1, src = -1;       // talkgroup (or called unit), calling unit
    int channel = -1;            // P25: identifier + channel; DMR: the logical channel number
    double freqHz = 0;           // 0 when the identifier update / channel table for it is not known yet
    int slot = -1;               // TDMA (P25 Phase 2 channel type or DMR): 0 or 1; -1 not TDMA
    bool tdma = false;
    bool emergency = false;
    bool encrypted = false;      // reported only
    int algId = -1;              // when known
    int priority = 4;            // service options priority (1 high ... 7 low; 4 default)
    bool explicitUpdate = false; // came from an update message (a repeat of a running call), not a new grant
    bool provoice = false;       // EDACS: the call is ProVoice (digital voice): logged, no decoder for it
    bool dataCall = false;       // MPT 1327: a data call (no speech)
    bool noPreempt = false;      // LTR / Passport: a word seen once only: it may start a call on a free radio, never take one from a running call
    std::string label;           // the talkgroup as the system writes it (EDACS AFS "02-071", LTR "0-05-123", MPT 1327 "5/1234"); empty: the number
};

struct IdenInfo {
    bool valid = false, vu = false, tdma = false;
    int chanType = 0, slots = 1;
    double baseHz = 0, spacingHz = 0, txOffsetHz = 0, bwHz = 0;
};

struct SiteInfo {
    bool valid = false;
    int wacn = -1, sysId = -1, rfss = -1, site = -1, nac = -1;
    int ccChannel = -1;
    double ccHz = 0;
    int ssc = 0;
    double lat = 0, lon = 0;     // only when the user put coordinates into the site table (the control channel does not carry them)
    bool hasPosition = false;
    double lastSec = 0;
    bool adjacent = false;
};

class SystemModel {
public:
    // One decoded TSBK from the control channel. Grants are appended to *grants (may be null); the return value is a short text for the message list ("" for nothing new).
    std::string onTsbk(const p25::Tsbk& t, int nac, double nowSec, std::vector<Grant>* grants);
    double channelFreq(int channel, int* slot = nullptr, bool* tdma = nullptr) const;   // 0 when unknown
    const SiteInfo& site() const { return site_; }
    const std::vector<SiteInfo>& adjacent() const { return adj_; }
    const IdenInfo& iden(int id) const { return idens_[id & 15]; }
    // The control channel and the others the system announces (secondary control channels, adjacent sites): the frequencies to try, current one first
    std::vector<double> controlCandidates() const;
    void setSitePosition(int sysId, int rfss, int site, double lat, double lon);
    uint64_t tsbks() const { return nTsbk_; }
    uint64_t grants() const { return nGrant_; }
    void clear();

private:
    SiteInfo site_;
    std::vector<SiteInfo> adj_;
    IdenInfo idens_[16];
    std::vector<double> secondary_;
    std::vector<std::pair<std::pair<int, std::pair<int, int>>, std::pair<double, double>>> positions_;
    uint64_t nTsbk_ = 0, nGrant_ = 0;
};

// Extra opcodes (TIA-102.AABC) next to the ones in p25_proto.h
constexpr int kOpGrpVGrantUpdExp = 0x03, kOpIdenUpTdma = 0x33, kOpSccb = 0x39;

// ------------------------------------------------------------------------------------------------ talkgroup list

struct TgEntry {
    int id = 0;
    std::string alpha, mode, desc, tag, category, list;   // short name, D / DE / T / A, description, tag (Law Dispatch ...), group, scan list
    int priority = 0;            // 0 none; 1 (highest) to 10
    bool lockout = false;
    bool listen = true;          // taken into the scan lists when scan-only is on
    bool encryptedMode() const { return !mode.empty() && mode.find('E') != std::string::npos; }
};

class TalkgroupList {
public:
    TgEntry* find(int id);
    const TgEntry* find(int id) const;
    TgEntry& upsert(int id);
    bool remove(int id);
    const std::vector<TgEntry>& entries() const { return v_; }
    void clear() { v_.clear(); }
    // CSV: header row decides the columns (case-insensitive; Decimal / DEC / TGID, Hex, Alpha Tag, Mode, Description, Tag, Category / Group, Priority, Lockout, List).
    // Quoted fields, "" escapes, CR / LF / CRLF accepted. Returns the number of rows taken; bad rows are counted in *skipped and described in err (first few).
    int importCsv(const std::string& text, std::string* err = nullptr, int* skipped = nullptr, bool merge = true);
    std::string exportCsv() const;
    // Scan lists: when scanOnly is on only talkgroups with listen set (and in an enabled list, if any list is enabled) are followed.
    void setScanOnly(bool on) { scanOnly_ = on; }
    bool scanOnly() const { return scanOnly_; }
    void enableList(const std::string& name, bool on);
    bool listEnabled(const std::string& name) const;
    std::vector<std::string> lists() const;
    struct Decision { bool follow = true; int priority = 0; const char* reason = ""; };
    Decision decide(int tg) const;

private:
    std::vector<TgEntry> v_;
    bool scanOnly_ = false;
    std::map<std::string, bool> enabled_;
};

// ------------------------------------------------------------------------------------------------ call log

struct CallRecord {
    uint64_t id = 0;
    int64_t wallTime = 0;        // Unix time of the start
    double startSec = 0, lastSec = 0, durSec = 0;
    bool active = false;
    bool dmr = false, group = true;
    int tg = -1, src = -1;
    int channel = -1; double freqHz = 0; int slot = -1;
    bool p25p2 = false;          // a P25 Phase 2 (TDMA) call: its voice is AMBE+2, not IMBE
    bool emergency = false, encrypted = false;
    SysKind kind = SysKind::P25;
    std::string label;           // the talkgroup as the system writes it (empty: the number)
    bool provoice = false;       // EDACS ProVoice: logged as ProVoice, no decoder for its voice
    bool followed = false;       // a slice or the radio was on it
    bool missed = false;         // not followed because it needed a wider capture or a second radio
    std::string note;            // why it was not followed: lockout, encrypted, busy, out of span ...
    std::string audioPath;       // the recording, when one was made
};

class CallLog {
public:
    CallRecord* byId(uint64_t id);
    const std::vector<CallRecord>& records() const { return v_; }   // oldest first
    CallRecord& add(const CallRecord& r);
    void trim(size_t keep);
    std::string exportCsv(const TalkgroupList* names = nullptr) const;
    void clear() { v_.clear(); next_ = 1; }

private:
    std::vector<CallRecord> v_;
    uint64_t next_ = 1;
};

// ------------------------------------------------------------------------------------------------ follower

struct Action {
    enum Kind { OpenSlice, CloseSlice, Retune, Listen, RecordStart, RecordStop, Notice, ReleaseRadio } kind = Notice;
    uint64_t callId = 0;
    int slice = -1;              // OpenSlice / CloseSlice / Listen: the follower's own slice number
    int radio = -1;              // Retune / RecordStart / ReleaseRadio: -1 the control radio itself, k >= 0 voice radio k. RecordStop: k, or -1
                                 // when the call was not on a voice radio or its radio is gone (key recordings by callId).
                                 // ReleaseRadio: the call on voice radio k ended and the radio is free (informational: voiceCall(k) says the same)
    double freqHz = 0;           // OpenSlice / Retune
    int slot = -1;               // Retune for a call: the call's TDMA slot (P25 Phase 2: the logical channel 0 / 1 to follow; DMR: the time slot), -1 none
    bool p25p2 = false;          // Retune for a call: a P25 Phase 2 TDMA voice channel (the receiver switches to Phase 2 on `slot`); a Retune without it
                                 // (another call, the way back to the control channel) is Phase 1 again
    std::string text;            // Notice; RecordStart: the file name stem
};

struct FollowConfig {
    int maxSlices = 2;           // simultaneous followed calls inside the capture (CPU limit)
    double hangSec = 2.0;        // a call with no grant repeat or voice for this long has ended
    double ccLossSec = 4.0;      // no control message for this long: the control channel is lost
    double ccDwellSec = 3.0;     // time spent on each candidate while looking for the control channel
    double returnTimeoutSec = 3.0;   // back on the control channel but nothing heard for this long: start looking
    double edgeGuardHz = 12500;  // a voice channel this close to the edge of the capture is not taken as inside
    double sliceBandwidthHz = 12500;
    bool allowRetune = true;     // the single radio may leave the control channel for a call
    bool record = true;
};

enum class FollowState { NoControl, ControlHold, Following, Hopped, Returning, Searching };
const char* followStateName(FollowState s);

class Follower {
public:
    explicit Follower(const FollowConfig& c = FollowConfig()) : cfg_(c) {}
    void setConfig(const FollowConfig& c) { cfg_ = c; }
    const FollowConfig& config() const { return cfg_; }
    // The capture: centre and span of what the radio delivers now. spanHz 0: only the tuned channel (every other frequency needs a retune).
    void setCapture(double centerHz, double spanHz) { centerHz_ = centerHz; spanHz_ = spanHz; }
    void setControlChannel(double hz, const std::vector<double>& candidates);
    // A control channel message arrived (any good TSBK)
    void onControlSeen(double now);
    // A grant (new, or a repeat of a running call). decision comes from the talkgroup list.
    void onGrant(const Grant& g, const TalkgroupList::Decision& d, double now, CallLog& log);
    // The slice (or the hopped radio) hears voice from the call, or its terminator
    void onVoice(uint64_t callId, double now);
    void onTerminator(uint64_t callId, double now);
    // a followed call turned out to be encrypted: it is marked in the log (once), its radio is released, and the grant updates that keep
    // coming for it (same talkgroup, channel and slot) neither retune a radio nor add rows until they stop for a while (a new call)
    void onEncrypted(uint64_t callId, double now, CallLog& log);
    // Time passes: ends calls, returns the radio, rotates through control channel candidates
    void tick(double now, CallLog& log);
    std::vector<Action> takeActions();
    FollowState state() const { return state_; }
    int activeSlices() const;
    uint64_t audibleCall() const;      // the call whose sound plays (highest priority, then oldest); 0 none
    uint64_t missedCalls() const { return missed_; }
    bool inSpan(double freqHz) const;
    double currentControlHz() const { return ccHz_; }
    double radioHz() const { return radioHz_; }   // where the single radio was last told to be (0: not told)
    // Voice radios: n other radios that follow calls outside the capture, so the control radio never leaves the control channel.
    // n = 0 (the default): the control radio hops for a call as before. Calls on radios that go away end at the next tick.
    void setVoiceRadios(int n);
    int voiceRadios() const { return (int)voice_.size(); }
    // Voice radio k is gone (unplugged, its tab closed): its call ends at the next tick (or endDropped) with note why, the radios above
    // it move down by one
    void dropVoiceRadio(int k, const char* why);
    void endDropped(CallLog& log);                     // end the calls of dropped radios now (RecordStop with radio -1)
    void endVoiceCalls(CallLog& log, const char* why); // end every call on a voice radio now (following is off); the radios stay linked
    uint64_t voiceCall(int k) const;        // the call on voice radio k; 0 free (or no such radio)
    int voiceRadioOf(uint64_t callId) const;   // the voice radio carrying the call; -1 none
    void reset();

private:
    struct EncMark { SysKind kind = SysKind::P25; bool dmr = false, group = true; int tg = -1, slot = -1; double freqHz = 0, lastSec = 0; };
    std::vector<EncMark> enc_;   // encrypted calls whose updates are still coming

    struct Slot { uint64_t callId = 0; int prio = 0; double freqHz = 0, startSec = 0, lastSec = 0; bool used = false, hopped = false; };
    std::vector<Slot> slots_;
    std::vector<Slot> voice_;          // one per voice radio
    std::vector<std::pair<Slot, std::string>> dropped_;   // calls on radios that went away: closed at the next tick
    FollowConfig cfg_;
    FollowState state_ = FollowState::NoControl;
    double centerHz_ = 0, spanHz_ = 0, ccHz_ = 0, radioHz_ = 0, homeHz_ = 0;
    std::vector<double> cands_;
    size_t candIdx_ = 0;
    double lastCcSec_ = -1e9, stateSince_ = 0, lastNow_ = 0, dwellStart_ = 0;
    std::vector<Action> out_;
    uint64_t missed_ = 0;
    std::vector<std::pair<double, double>> noticed_;   // out-of-span frequencies already announced
    void note(const std::string& s) { Action a; a.text = s; out_.push_back(a); }
    void close(Slot& s, CallLog& log, double now, const char* why);
    void endVoice(Slot& s, int radio, CallLog& log, const char* why);
    void goHome(double now);
    void setState(FollowState s, double now) { if (state_ != s) { state_ = s; stateSince_ = now; } }
    Slot* freeSlot();
};

// ------------------------------------------------------------------------------------------------ DMR Tier III

// The Tier III channel grants as CSBKs (ETSI TS 102 361-4). The opcode numbers are kept in one table; see docs/trunking.md for what is verified.
struct DmrOpcodes { int aloha = 0x19, pvGrant = 0x30, tvGrant = 0x31, btvGrant = 0x32; };   // PV_GRANT 48, TV_GRANT 49, BTV_GRANT 50
struct DmrLcn { int lcn = 0; double freqHz = 0; };
class DmrModel {
public:
    void setOpcodes(const DmrOpcodes& o) { ops_ = o; }
    void setChannel(int lcn, double freqHz);    // the user's logical channel table (the grants carry channel numbers only)
    double lcnFreq(int lcn) const;
    // csbk: opcode, feature ID, 8 data bytes, most significant bit first: physical channel number (12 bits), timeslot (1), late entry (1),
    // emergency (1), offset (1), then the 24-bit target and the 24-bit source (TS 102 361-4 channel grant layout).
    bool onCsbk(int opcode, int fid, const uint8_t data[8], double nowSec, Grant& out);
    uint64_t grants() const { return n_; }

private:
    DmrOpcodes ops_;
    std::map<int, double> lcn_;
    uint64_t n_ = 0;
};

// ------------------------------------------------------------------------------------------------ NXDN

// NXDN trunking (Type C): the control channel's voice channel assignments (VCALL_ASSGN, NXDN TS 1-D). They carry a 10-bit channel number;
// what frequency a number means comes from the user's channel table (systems using direct frequency assignment are not followed).
class NxdnModel {
public:
    void setChannel(int ch, double freqHz);
    double channelFreq(int ch) const;
    // callType: 0 broadcast, 1 group, 4 individual, 6 interconnect; ccOption bit 7 emergency
    bool onAssignment(int callType, int src, int dst, int channel, int ccOption, Grant& out);
    uint64_t grants() const { return n_; }

private:
    std::map<int, double> ch_;
    uint64_t n_ = 0;
};

// ------------------------------------------------------------------------------------------------ analog trunking (EDACS, LTR, MPT 1327)

// The user's channel table: a channel number (EDACS LCN, LTR repeater number, MPT 1327 channel) -> frequency. Typed or pasted as CSV
// "LCN,frequency" (one per line; also "LCN=frequency", tabs, semicolons or spaces between; the frequency in MHz, or in Hz when it is above
// 6000; a header row and # comments are skipped). MPT 1327 also has a formula: base + number x spacing for numbers not in the table.
class ChannelTable {
public:
    int parse(const std::string& text, int maxNumber, std::string* err = nullptr, int* skipped = nullptr);   // replaces the table; rows taken
    std::string csv() const;                     // "LCN,frequency MHz" lines
    void set(int n, double hz) { if (hz > 0) t_[n] = hz; else t_.erase(n); }
    double freq(int n) const;                    // 0 unknown
    void setFormula(double baseHz, double spacingHz) { base_ = baseHz; spacing_ = spacingHz; }
    double baseHz() const { return base_; }
    double spacingHz() const { return spacing_; }
    bool known() const { return !t_.empty() || (base_ > 0 && spacing_ > 0); }
    size_t size() const { return t_.size(); }
    const std::map<int, double>& entries() const { return t_; }
    void clear() { t_.clear(); base_ = spacing_ = 0; }
private:
    std::map<int, double> t_;
    double base_ = 0, spacing_ = 0;
};

// EDACS: the control channel's frames (dect2/edacs/edacs_proto.h, after dsd-fme) into grants. Standard / networked group calls carry the
// 11-bit AFS group, EA ones a 16-bit group; individual calls the called and calling radio. A console drop ends the call on its LCN.
class EdacsModel {
public:
    ChannelTable lcns;
    int mode = 0;                // 0 standard / networked, 1 extended addressing (EA)
    bool esk = false;            // the command bits are XORed with the ESK 0xA0
    // one frame (its two messages): a grant in g (true); *dropLcn: a console drop of that LCN (-1 none); text: the frame in words
    bool onFrame(uint32_t m1, uint32_t m2, Grant& g, int* dropLcn = nullptr, std::string* text = nullptr);
    int sysId() const { return sysId_; }
    int site() const { return site_; }
    uint64_t grants() const { return n_; }
    uint64_t messages() const { return msgs_; }
    uint64_t understood() const { return known_; }   // frames the message set made sense of (any kind, not only grants)
    void reset() { sysId_ = site_ = -1; }
private:
    int sysId_ = -1, site_ = -1;
    uint64_t n_ = 0, msgs_ = 0, known_ = 0;
};

// LTR: the subaudible words into calls. A word (area, go-to, home, id, free) with an id of a call says that talkgroup area-home-id talks
// on repeater go-to; the turn-off word (go-to 31) ends it. Only words whose checksum matched get here (SDRTrunk CRCLTR, ltr_proto.h).
// A word seen once may start a call on a free radio; it takes a radio from a running call only when the next word repeats it.
// Passport: a call word (type 0 / 2, or 1 with an LCN below 1792) puts the 16-bit group on that LCN; type 1 with LCN 2047 ends it. The LCN
// becomes a frequency through the user's table, or else the site's band from the idle words (base + LCN x 12.5 kHz, SDRTrunk PassportBand).
class LtrModel {
public:
    ChannelTable repeaters;      // LTR repeater numbers, or Passport LCNs
    // a grant in g (true), or *endedTg >= 0: the turn-off of that talkgroup
    bool onWord(int area, int gotoCh, int home, int id, int freeCh, double now, Grant& g, int* endedTg);
    bool onPassport(int dcc, int lcn, int site, int group, int type, int freeCh, double now, Grant& g, int* endedTg);
    int passportBand() const { return band_; }
    uint64_t grants() const { return n_; }
    void reset() { band_ = -1; last_ = -1; }
private:
    int band_ = -1;
    int64_t last_ = -1;          // the previous word (its fields as one key): a grant seen on two consecutive words may preempt
    uint64_t n_ = 0;
};

// LTR coverage. LTR has no control channel: a repeater sends its words only while it is busy. So every repeater of the system is watched:
// those inside the radio's capture at once (LtrReceiver::setWatch), and when they do not all fit, the capture sweeps through groups of
// them, dwelling long enough for a few words (one every 0.17 s) on each and staying while one of the group is busy.
struct CoverageGroup {
    double mainHz = 0;                 // the channel the receiver decodes (the radio's centre is mainHz - mainOffsetHz)
    std::vector<double> others;        // the other repeaters inside that capture
};
// freqs: the repeaters; a channel is inside when it is within spanHz / 2 - guardHz of the radio's centre
std::vector<CoverageGroup> planCoverage(std::vector<double> freqs, double spanHz, double mainOffsetHz, double guardHz = 15000);

class LtrSweep {
public:
    double dwellSec = 1.2;             // on a quiet group (about 7 words' time)
    double maxStaySec = 8.0;           // a busy group keeps the sweep at most this long (unless this radio is on a call there)
    void setPlan(const std::vector<CoverageGroup>& g);
    const std::vector<CoverageGroup>& plan() const { return plan_; }
    // the group to tune to now, or -1 to stay where it is. active: a repeater of the current group sent a good word just now;
    // following: the receiver is on a call (it does not move then)
    int step(double now, bool active, bool following);
    int current() const { return idx_; }
    bool sweeping() const { return plan_.size() > 1; }
private:
    std::vector<CoverageGroup> plan_;
    int idx_ = -1;
    double since_ = -1e9;
    double entered_ = -1e9;
};

// MPT 1327: GTC (go to channel) on the control channel into grants. The called address (prefix / ident) is the talkgroup, the caller's
// ident the source. Channel numbers become frequencies through the user's table or base + number x spacing. The idents say what kind of
// call it is (section 5.4, Table 4-1): ALLI a system-wide call, a PSTN / PABX gateway ident a telephone call (the radio is then the party
// logged). Whether a user ident (1 - 8100) is a group or a unit is the system's fleet plan, not in the codeword: idents from groupMin to
// groupMax are taken as groups (all of them by default, as SDRTrunk does), the others as individual calls.
class MptModel {
public:
    ChannelTable channels;
    int groupMin = 1, groupMax = 8100;
    bool onMessage(const std::string& type, int calledPfix, int calledIdent, int callerIdent, int channel, bool dataCall, bool traffic, Grant& g);
    uint64_t grants() const { return n_; }
private:
    uint64_t n_ = 0;
};

// ------------------------------------------------------------------------------------------------ controller

struct ControllerStatus {
    FollowState state = FollowState::NoControl;
    uint64_t tsbkGood = 0, tsbkBad = 0;
    double crcOkPct = 0;         // control channel message success rate
    bool controlLocked = false;
    int activeCalls = 0;
    uint64_t missed = 0;
};

class Controller {
public:
    Controller();
    SystemModel sys;
    TalkgroupList tgs;
    CallLog log;
    Follower follower;
    DmrModel dmr;
    NxdnModel nxdn;
    EdacsModel edacs;
    LtrModel ltr;
    MptModel mpt;

    // From the P25 receiver: a good TSBK (nac = the NID's), or a failed one (counts toward the success rate)
    void onTsbk(const p25::Tsbk& t, int nac, double nowSec);
    void onTsbkBad(double nowSec);
    void onDmrCsbk(int opcode, int fid, const uint8_t data[8], double nowSec);
    void onNxdnAssignment(int callType, int src, int dst, int channel, int ccOption, double nowSec);
    void onEdacs(uint32_t m1, uint32_t m2, double nowSec);                          // one control channel frame (its two messages)
    void onLtr(int area, int gotoCh, int home, int id, int freeCh, double nowSec);  // one word heard by the monitoring receiver
    void onPassport(int dcc, int lcn, int site, int group, int type, int freeCh, double nowSec);   // one Passport word
    void onMpt(const std::string& type, int calledPfix, int calledIdent, int callerIdent, int channel, bool dataCall, bool traffic, double nowSec);
    void onControlSeen(double nowSec);   // a good control channel message that is not a grant (keeps the follower's control lock)
    // One talkgroup list per system: tgs is the list of the system being followed (LTR 3017, EDACS 3017 and P25 3017 are not the same
    // talkgroup). Each system's messages select its list; the first system seen keeps the list that was there.
    void selectSystem(SysKind k);
    SysKind system() const { return cur_; }
    bool systemChosen() const { return curSet_; }
    TalkgroupList& listOf(SysKind k) { return k == cur_ ? tgs : others_[k]; }
    void setCapture(double centerHz, double spanHz) { follower.setCapture(centerHz, spanHz); }
    void tick(double nowSec);
    std::vector<Action> takeActions() { return follower.takeActions(); }
    void onVoice(uint64_t callId, double now) { follower.onVoice(callId, now); }
    void onTerminator(uint64_t callId, double now) { follower.onTerminator(callId, now); }
    void onEncrypted(uint64_t callId, double now) { follower.onEncrypted(callId, now, log); }
    // the calls on radios that go away end at once (their RecordStop is in the next takeActions)
    void setVoiceRadios(int n) { follower.setVoiceRadios(n); follower.endDropped(log); }
    void dropVoiceRadio(int k, const char* why) { follower.dropVoiceRadio(k, why); follower.endDropped(log); }
    void endVoiceCalls(const char* why) { follower.endVoiceCalls(log, why); }
    ControllerStatus status(double nowSec) const;
    // P25 Phase 2 bursts are scrambled with WACN, system and NAC: a Phase 2 grant waits (at most 3 s) until all three are known
    bool phase2Ready() const { const SiteInfo& s = sys.site(); return s.wacn >= 0 && s.sysId >= 0 && s.nac >= 0; }
    size_t phase2Waiting() const { return p2Pending_.size(); }
    // goes up each time the known WACN / system / NAC change: a radio on a Phase 2 call is seeded again
    uint64_t phase2SeedGen() const { return p2Gen_; }
    std::vector<std::string> messages;   // newest last, at most 200
    void clear();

    // The live "now talking" view: calls that are active
    std::vector<const CallRecord*> nowTalking() const;

private:
    uint64_t good_ = 0, bad_ = 0;
    double lastGood_ = -1e9;
    SysKind cur_ = SysKind::P25;
    bool curSet_ = false;
    std::map<SysKind, TalkgroupList> others_;   // the lists of the systems not followed now
    void adopt(SysKind k) { if (!curSet_) { cur_ = k; curSet_ = true; } else if (k != cur_) selectSystem(k); }
    struct P2Pending { Grant g; double t; };
    std::vector<P2Pending> p2Pending_;
    uint64_t p2Gen_ = 0;
    int p2Wacn_ = -1, p2Sys_ = -1, p2Nac_ = -1;
    void pushMsg(const std::string& s);
    void dispatch(const std::vector<Grant>& gs, double now);
};

// A TSBK generator for tests and for the demo: the messages a control channel sends, as Tsbk records (they go through tsbkBits / tsbkParse like the real ones)
namespace gen {
p25::Tsbk idenUp(int id, double baseHz, double spacingHz, double txOffsetHz, double bwHz);          // IDEN_UP (0x3D)
p25::Tsbk idenUpVu(int id, double baseHz, double spacingHz, double txOffsetHz, int bwCode);          // IDEN_UP_VU (0x34)
p25::Tsbk idenUpTdma(int id, int chanType, double baseHz, double spacingHz, double txOffsetHz);     // IDEN_UP_TDMA (0x33)
p25::Tsbk netSts(int wacn, int sysId, int ccChannel, int ssc);
p25::Tsbk rfssSts(int sysId, int rfss, int site, int ccChannel, int ssc);
p25::Tsbk adjSts(int sysId, int rfss, int site, int ccChannel, int ssc);
p25::Tsbk sccb(int rfss, int site, int ch1, int ch2);
p25::Tsbk grpVGrant(int svcOpts, int channel, int tg, int src);
p25::Tsbk uuVGrant(int channel, int dst, int src);
p25::Tsbk grpVGrantUpd(int ch1, int tg1, int ch2, int tg2);
p25::Tsbk grpVGrantUpdExp(int svcOpts, int chT, int chR, int tg);
}

} // namespace trunk
} // namespace dect2
