#include "dect2/transcribe/callsign.h"
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

namespace dect2 {

namespace {

struct Tel { const char* name; const char* icao; };
// Radio names of airlines (ICAO Doc 8585 telephony designators), the ones most heard in Europe, the Gulf and North America, plus freight.
const Tel kTel[] = {
    {"lufthansa", "DLH"}, {"speedbird", "BAW"}, {"ryanair", "RYR"}, {"easy", "EZY"}, {"easyjet", "EZY"}, {"emirates", "UAE"}, {"air france", "AFR"},
    {"airfrans", "AFR"}, {"klm", "KLM"}, {"delta", "DAL"}, {"american", "AAL"}, {"united", "UAL"}, {"cactus", "AWE"}, {"skywest", "SKW"},
    {"qatari", "QTR"}, {"etihad", "ETD"}, {"turkish", "THY"}, {"wizz", "WZZ"}, {"wizz air", "WZZ"}, {"eurowings", "EWG"}, {"finnair", "FIN"},
    {"finn air", "FIN"}, {"scandinavian", "SAS"}, {"swiss", "SWR"}, {"austrian", "AUA"}, {"iberia", "IBE"}, {"vueling", "VLG"}, {"itarrow", "ITY"},
    {"shamrock", "EIN"}, {"virgin", "VIR"}, {"jetblue", "JBU"}, {"jet blue", "JBU"}, {"southwest", "SWA"}, {"fedex", "FDX"}, {"ups", "UPS"},
    {"cargolux", "CLX"}, {"air baltic", "BTI"}, {"airbaltic", "BTI"}, {"baltic", "BTI"}, {"lot", "LOT"}, {"ethiopian", "ETH"}, {"saudia", "SVA"},
    {"saudi", "SVA"}, {"gulf air", "GFA"}, {"gulf", "GFA"}, {"oman air", "OMA"}, {"kuwaiti", "KAC"}, {"arabia", "ABY"}, {"flydubai", "FDB"},
    {"fly dubai", "FDB"}, {"skydubai", "FZA"}, {"sky dubai", "FZA"}, {"air india", "AIC"}, {"indigo", "IGO"}, {"singapore", "SIA"}, {"cathay", "CPA"},
    {"qantas", "QFA"}, {"korean air", "KAL"}, {"japan air", "JAL"}, {"all nippon", "ANA"}, {"air canada", "ACA"}, {"rouge", "ROU"}, {"westjet", "WJA"},
    {"nor shuttle", "NAX"}, {"norwegian", "NAX"}, {"nordic", "NSZ"}, {"transavia", "TRA"}, {"tui", "TUI"}, {"tui fly", "TUI"}, {"jet2", "EXS"},
    {"channex", "EXS"}, {"brussels", "BEL"}, {"sabena", "BEL"}, {"tap", "TAP"}, {"air portugal", "TAP"}, {"aegean", "AEE"}, {"alitalia", "AZA"},
    {"neos", "NOS"}, {"condor", "CFG"}, {"discover", "DLA"}, {"edelweiss", "EDW"}, {"luxair", "LGL"}, {"lux", "LGL"}, {"aurigny", "AUR"},
    {"loganair", "LOG"}, {"citiflyer", "CFE"}, {"flybe", "BEE"}, {"blue islands", "BCI"}, {"smartlynx", "ESD"}, {"smart lynx", "ESD"}, {"avion express", "AEX"},
    {"get jet", "GJT"}, {"getjet", "GJT"}, {"dhl", "DHK"}, {"euro cargo", "BCS"}, {"ascend", "ASD"}, {"atlas", "GTI"}, {"giant", "GTI"},
    {"sunrise", "TVS"}, {"travel service", "TVS"}, {"czech", "CSA"}, {"slovak", "SQ"}, {"malev", "MAH"}, {"carpatair", "KRP"}, {"tarom", "ROT"},
    {"bulgaria air", "LZB"}, {"balkan", "LZB"}, {"croatia", "CTN"}, {"adria", "ADR"}, {"air serbia", "ASL"}, {"jat", "JAT"}, {"blue air", "BMS"},
    {"egyptair", "MSR"}, {"royal jordanian", "RJA"}, {"jordanian", "RJA"}, {"middle east", "MEA"}, {"cedar jet", "MEA"}, {"el al", "ELY"},
    {"israeli", "ELY"}, {"pegasus", "PGT"}, {"sunexpress", "SXS"}, {"sun express", "SXS"}, {"anadolu", "AJA"}, {"azerbaijan", "AHY"}, {"aeroflot", "AFL"},
    {"fly arystan", "FAS"}, {"air astana", "KZR"}, {"astanaline", "KZR"}, {"uzbek", "UZB"}, {"jazeera", "JZR"}, {"salam air", "OMS"},
    {"air arabia", "ABY"}, {"air ethiopia", "ETH"}, {"kenya", "KQA"}, {"springbok", "SAA"}, {"safair", "SFR"}, {"airlink", "LNK"}, {"royal air maroc", "RAM"},
    {"air algerie", "DAH"}, {"tunisair", "TAR"}, {"nigeria", "NGA"}, {"air france", "AFR"}, {"hop", "HOP"}, {"corsair", "CRL"}, {"french blue", "FBU"},
    {"air europa", "AEA"}, {"europa", "AEA"}, {"binter", "IBB"}, {"canaryfly", "IBB"}, {"air nostrum", "ANE"}, {"volotea", "VOE"}, {"level", "LVL"},
    {"canada", "ACA"}, {"alaska", "ASA"}, {"hawaiian", "HAL"}, {"spirit wings", "NKS"}, {"frontier flight", "FFT"}, {"jetblue", "JBU"}, {"allegiant", "AAY"},
    {"sun country", "SCX"}, {"envoy", "ENY"}, {"republic", "RPA"}, {"compass rose", "CPZ"}, {"speedbird", "BAW"}, {"shuttle america", "SCX"},
    {"mesa", "ASH"}, {"air wisconsin", "AWI"}, {"endeavor", "EDV"}, {"piedmont", "PDT"}, {"psa", "JIA"}, {"cargo", "CRG"}, {"fedex", "FDX"},
    {"bluedart", "BDA"}, {"nippon cargo", "NCA"}, {"cargolux italia", "CLX"}, {"silkway", "AZG"}, {"silk way", "AZG"}, {"ethiopian cargo", "ETH"},
    {"qatari cargo", "QAC"}, {"emirates sky cargo", "UAE"}, {"emirates skycargo", "UAE"},
};

const char* kPhon[][2] = {
    {"alpha", "A"}, {"alfa", "A"}, {"bravo", "B"}, {"charlie", "C"}, {"delta", "D"}, {"echo", "E"}, {"foxtrot", "F"}, {"golf", "G"}, {"hotel", "H"},
    {"india", "I"}, {"juliet", "J"}, {"juliett", "J"}, {"kilo", "K"}, {"lima", "L"}, {"mike", "M"}, {"november", "N"}, {"oscar", "O"}, {"papa", "P"},
    {"quebec", "Q"}, {"romeo", "R"}, {"sierra", "S"}, {"tango", "T"}, {"uniform", "U"}, {"victor", "V"}, {"whiskey", "W"}, {"whisky", "W"},
    {"xray", "X"}, {"yankee", "Y"}, {"zulu", "Z"},
};

const char* kDigit[][2] = {
    {"zero", "0"}, {"one", "1"}, {"two", "2"}, {"three", "3"}, {"tree", "3"}, {"four", "4"}, {"fower", "4"}, {"five", "5"}, {"fife", "5"},
    {"six", "6"}, {"seven", "7"}, {"eight", "8"}, {"nine", "9"}, {"niner", "9"},
};
const char* kTeens[][2] = {{"ten", "10"}, {"eleven", "11"}, {"twelve", "12"}, {"thirteen", "13"}, {"fourteen", "14"}, {"fifteen", "15"},
                           {"sixteen", "16"}, {"seventeen", "17"}, {"eighteen", "18"}, {"nineteen", "19"}};
const char* kTens[][2] = {{"twenty", "2"}, {"thirty", "3"}, {"forty", "4"}, {"fifty", "5"}, {"sixty", "6"}, {"seventy", "7"}, {"eighty", "8"}, {"ninety", "9"}};

// Registration prefixes (country marks) a spoken letters-only registration is accepted with.
const char* kRegPrefix[] = {"G", "D", "F", "I", "EI", "HB", "OO", "PH", "OE", "SE", "LN", "OY", "OH", "ES", "YL", "LY", "SP", "OK", "OM", "HA", "YR", "LZ",
                            "TC", "SX", "EC", "CS", "A6", "A7", "HZ", "VH", "ZS", "JA", "VT", "9V", "9H", "RA", "UR", "4X", "SU", "YK", "9K", "A9", "S2"};

std::string lowerAlnum(const std::string& s) {
    std::string o;
    for (unsigned char c : s) o += (char)std::tolower(c);
    return o;
}

struct Word { std::string w; std::string raw; };   // lower case, and as written

std::vector<Word> words(const std::string& text) {
    std::vector<Word> v;
    std::string cur, rawCur;
    auto push = [&] { if (!cur.empty()) { v.push_back({cur, rawCur}); cur.clear(); rawCur.clear(); } };
    for (size_t i = 0; i < text.size(); i++) {
        const unsigned char c = (unsigned char)text[i];
        if (std::isalnum(c)) { cur += (char)std::tolower(c); rawCur += (char)c; }
        else if (c == '\'' && !cur.empty() && i + 1 < text.size() && std::isalpha((unsigned char)text[i + 1])) { /* "x'ray" style: drop */ }
        else push();
    }
    push();
    // "x ray" > "xray"
    for (size_t i = 0; i + 1 < v.size(); i++) if (v[i].w == "x" && v[i + 1].w == "ray") { v[i].w = "xray"; v[i].raw = "xray"; v.erase(v.begin() + (long)i + 1); }
    return v;
}

bool isDigits(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); }); }

// A run of digits spoken as words or written as numbers, at word i; returns how many words it used and the digits.
size_t readDigits(const std::vector<Word>& w, size_t i, std::string& out, size_t maxDigits) {
    size_t used = 0;
    out.clear();
    while (i + used < w.size() && out.size() < maxDigits) {
        const std::string& x = w[i + used].w;
        std::string d;
        size_t take = 1;
        if (isDigits(x) && out.size() + x.size() <= maxDigits) d = x;
        else {
            for (auto& p : kDigit) if (x == p[0]) d = p[1];
            if (d.empty()) for (auto& p : kTeens) if (x == p[0]) d = p[1];
            if (d.empty()) {
                for (auto& p : kTens) if (x == p[0]) {
                    d = p[1];
                    if (i + used + 1 < w.size()) {   // "twenty three"
                        for (auto& q : kDigit) if (w[i + used + 1].w == q[0] && q[1][0] != '0') { d += q[1]; take = 2; }
                    }
                    if (take == 1) d += "0";
                }
            }
        }
        if (d.empty() || out.size() + d.size() > maxDigits) break;
        out += d;
        used += take;
    }
    return used;
}

// A letter spoken as a phonetic word, or written alone ("a"): "" when it is neither.
std::string readLetter(const std::string& x) {
    for (auto& p : kPhon) if (x == p[0]) return p[1];
    if (x.size() == 1 && std::isalpha((unsigned char)x[0]) && x != "a" && x != "i") return std::string(1, (char)std::toupper((unsigned char)x[0]));
    return "";
}

const std::map<std::string, std::string>& telMap() {
    static const std::map<std::string, std::string> m = [] {
        std::map<std::string, std::string> r;
        for (const auto& t : kTel) r.emplace(t.name, t.icao);
        return r;
    }();
    return m;
}

bool prefixOk(const std::string& s) {
    for (const char* p : kRegPrefix) {
        const size_t n = strlen(p);
        if (s.size() >= n && s.compare(0, n, p) == 0) return true;
    }
    return false;
}

std::string join(const std::vector<Word>& w, size_t a, size_t b) {
    std::string o;
    for (size_t i = a; i < b; i++) { if (i > a) o += ' '; o += w[i].raw; }
    return o;
}

}  // namespace

std::string telephonyToIcao(const std::string& name) {
    const auto it = telMap().find(name);
    return it == telMap().end() ? std::string() : it->second;
}
size_t telephonyCount() { return telMap().size(); }

std::string icaoToTelephony(const std::string& icao) {
    if (icao.size() != 3) return {};
    // The table is keyed by the spoken name, and aliases can share a designator. It is a std::map, so walking it is in alphabetical order
    // of the spoken name; keeping the first match makes the answer stable between calls and between builds.
    static const std::map<std::string, std::string> rev = [] {
        std::map<std::string, std::string> m;
        for (const auto& kv : telMap()) m.emplace(kv.second, kv.first);   // emplace keeps the first name seen for a designator
        return m;
    }();
    const auto it = rev.find(icao);
    if (it == rev.end()) return {};
    // Capitalise each word, so it reads as a name in the prompt rather than as lower-case noise.
    std::string o = it->second;
    bool start = true;
    for (char& c : o) {
        if (start && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        start = (c == ' ');
    }
    return o;
}

namespace {

// Aviation reads digits one at a time, and says two of them its own way.
const char* kDigitWord[10] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "niner"};

void appendDigits(std::string& out, const std::string& digits) {
    for (char c : digits) {
        if (c < '0' || c > '9') continue;
        if (!out.empty() && out.back() != ' ') out += ' ';
        out += kDigitWord[c - '0'];
    }
}

}  // namespace

std::string spokenFlight(const std::string& callsign) {
    if (callsign.empty()) return {};
    std::string cs;
    for (char c : callsign) if (!std::isspace((unsigned char)c)) cs += (char)std::toupper((unsigned char)c);
    if (cs.size() < 3) return cs;
    // A registration (anything that is not three letters then a digit) is read out as it is written.
    bool threeLetters = true;
    for (int i = 0; i < 3; i++) if (!std::isupper((unsigned char)cs[i])) threeLetters = false;
    if (!threeLetters || cs.size() == 3 || !std::isdigit((unsigned char)cs[3])) return cs;
    const std::string name = icaoToTelephony(cs.substr(0, 3));
    const std::string tail = cs.substr(3);
    if (name.empty()) return cs.substr(0, 3) + " " + tail;   // no radio name: letters, then the number
    return name + " " + tail;
}

std::string spokenRunway(const std::string& ident) {
    std::string digits, side;
    for (char c : ident) {
        if (std::isdigit((unsigned char)c)) digits += c;
        else if (c == 'L' || c == 'l') side = "left";
        else if (c == 'R' || c == 'r') side = "right";
        else if (c == 'C' || c == 'c') side = "center";
        else if (std::isspace((unsigned char)c)) continue;
        else return {};                                      // not a runway identifier
    }
    if (digits.empty() || digits.size() > 2) return {};
    const int n = std::atoi(digits.c_str());
    if (n < 1 || n > 36) return {};
    std::string out = "runway ";
    // "09" is said "zero niner", "9" is said "niner": keep whichever the identifier itself used.
    std::string words;
    appendDigits(words, digits);
    out += words;
    if (!side.empty()) out += " " + side;
    return out;
}

std::vector<CallsignHit> findCallsigns(const std::string& text) {
    std::vector<CallsignHit> hits;
    const std::vector<Word> w = words(text);
    std::vector<bool> used(w.size(), false);
    auto add = [&](CallsignHit::Kind k, const std::string& key, size_t a, size_t b) {   // words [a, b)
        CallsignHit h;
        h.kind = k; h.key = key; h.spoken = join(w, a, b); h.firstWord = a; h.lastWord = b - 1; h.nWords = w.size();
        hits.push_back(h);
        for (size_t i = a; i < b; i++) used[i] = true;
    };
    for (size_t i = 0; i < w.size();) {
        if (used[i]) { i++; continue; }
        // 1. telephony name (two words first), then the number and up to two letters
        std::string icao;
        size_t nameWords = 0;
        if (i + 1 < w.size()) {
            const std::string two = w[i].w + " " + w[i + 1].w;
            icao = telephonyToIcao(two);
            if (!icao.empty()) nameWords = 2;
        }
        if (icao.empty()) { icao = telephonyToIcao(w[i].w); if (!icao.empty()) nameWords = 1; }
        // 2. an ICAO designator as written: three capital letters then digits ("DLH 123", "DLH123")
        size_t at = i + nameWords;
        if (icao.empty() && w[i].raw.size() >= 3) {
            const std::string& r = w[i].raw;
            size_t k = 0;
            while (k < r.size() && std::isupper((unsigned char)r[k])) k++;
            if (k == 3 && (r.size() == 3 || std::isdigit((unsigned char)r[3]))) {
                icao = r.substr(0, 3);
                if (r.size() > 3) {   // "DLH123" is one word
                    std::string tail = r.substr(3);
                    size_t d = 0;
                    while (d < tail.size() && std::isdigit((unsigned char)tail[d]) && d < 4) d++;
                    std::string num = tail.substr(0, d), suf = tail.substr(d);
                    while (num.size() > 1 && num[0] == '0') num.erase(0, 1);
                    bool sufOk = suf.size() <= 2;
                    for (unsigned char c : suf) sufOk &= std::isupper(c) != 0;
                    if (!num.empty() && sufOk) { add(CallsignHit::Flight, icao + num + suf, i, i + 1); i++; continue; }
                    icao.clear();
                } else nameWords = 1, at = i + 1;
            }
        }
        if (!icao.empty() && nameWords) {
            std::string num;
            const size_t n = readDigits(w, at, num, 4);
            if (n) {
                size_t end = at + n;
                std::string suf;
                for (int k = 0; k < 2 && end < w.size(); k++) {   // "...one two three alpha bravo"
                    const std::string l = readLetter(w[end].w);
                    if (l.empty() || used[end]) break;
                    // a lone "a" / "i" are words; other single letters and phonetic words count
                    suf += l;
                    end++;
                }
                while (num.size() > 1 && num[0] == '0') num.erase(0, 1);
                add(CallsignHit::Flight, icao + num + suf, i, end);
                i = end;
                continue;
            }
        }
        // 3. a written registration: N123AB, D-ABCD
        {
            const std::string& r = w[i].raw;
            if (r.size() >= 3 && r.size() <= 6 && (r[0] == 'N' || r[0] == 'n') && std::isdigit((unsigned char)r[1])) {
                size_t d = 1;
                while (d < r.size() && std::isdigit((unsigned char)r[d])) d++;
                bool ok = d <= 5;
                for (size_t k = d; k < r.size(); k++) ok &= std::isalpha((unsigned char)r[k]) != 0;
                if (ok && r.size() - d <= 2 && std::isupper((unsigned char)r[0]) ) { std::string key; for (char c : r) key += (char)std::toupper((unsigned char)c); add(CallsignHit::Reg, key, i, i + 1); i++; continue; }
            }
        }
        // 4. spoken registration: "november one two three alpha bravo", or a country prefix then letters and digits
        {
            std::string reg;
            size_t j = i;
            while (j < w.size() && reg.size() < 6 && !used[j]) {
                std::string d;
                const size_t n = readDigits(w, j, d, 6 - reg.size());
                if (n) { reg += d; j += n; continue; }
                const std::string l = readLetter(w[j].w);
                if (l.empty()) break;
                reg += l; j++;
            }
            if (j > i) {
                bool nreg = reg.size() >= 3 && reg[0] == 'N' && std::isdigit((unsigned char)reg[1]) && w[i].w == "november";
                if (nreg) {
                    // N + up to 5 digits + up to 2 letters
                    size_t d = 1;
                    while (d < reg.size() && std::isdigit((unsigned char)reg[d])) d++;
                    bool ok = reg.size() - d <= 2;
                    if (ok) { add(CallsignHit::Reg, reg, i, j); i = j; continue; }
                } else if (reg.size() >= 5 && j - i >= 4 && prefixOk(reg)) {
                    bool phon = false;
                    for (size_t k = i; k < j; k++) for (auto& p : kPhon) if (w[k].w == p[0]) phon = true;
                    if (phon) { add(CallsignHit::Reg, reg, i, j); i = j; continue; }
                }
            }
        }
        i++;
    }
    return hits;
}

std::string guessSpeaker(const std::vector<CallsignHit>& hits) {
    if (hits.empty()) return "";
    const CallsignHit& a = hits.front();
    const CallsignHit& z = hits.back();
    if (a.firstWord <= 1) return "ATC";
    if (z.lastWord + 2 >= z.nWords) return "Aircraft";
    return "";
}

}  // namespace dect2
