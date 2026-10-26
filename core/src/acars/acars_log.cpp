#include "dect2/acars/acars_log.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace dect2 {

namespace {

struct LabelCat { const char* label; char cat; };
const LabelCat kLabelCats[] = {
#include "acars_label_cat.inc"
};

bool isAlnum(unsigned char c) { return std::isalnum(c) != 0; }

std::string upper(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = (char)std::toupper((unsigned char)c);
    return o;
}

bool hasWord(const std::string& up, const char* w) { return up.find(w) != std::string::npos; }

// Binary-looking text: mostly punctuation, one long hex blob, or a long base64-like run.
bool binaryish(const std::string& t) {
    size_t good = 0, longest = 0, run = 0;
    bool hexOnly = true;
    for (unsigned char c : t) {
        if (std::isalnum(c) || c == ' ' || c == '\n') good++;
        if (isAlnum(c) || c == '+' || c == '/' || c == '=') { run++; longest = std::max(longest, run); } else run = 0;
        if (!(std::isdigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) hexOnly = false;
    }
    if (good * 10 < t.size() * 6) return true;
    if (hexOnly && t.size() > 24) return true;
    return longest > 60;
}

// Readable words: at least 6 letters and not binary-looking.
bool readable(const std::string& t) {
    size_t letters = 0;
    for (unsigned char c : t) if (std::isalpha(c)) letters++;
    return letters >= 6 && !binaryish(t);
}

}  // namespace

const char* acarsCatKey(AcarsCat c) {
    switch (c) {
    case AcarsCat::Free: return "free";
    case AcarsCat::Crew: return "crew";
    case AcarsCat::Ops: return "ops";
    case AcarsCat::Company: return "company";
    case AcarsCat::Maint: return "maint";
    case AcarsCat::Fault: return "fault";
    default: return "ignore";
    }
}

const char* acarsCatName(AcarsCat c) {
    switch (c) {
    case AcarsCat::Free: return "Free text";
    case AcarsCat::Crew: return "Crew";
    case AcarsCat::Ops: return "Operations";
    case AcarsCat::Company: return "Company";
    case AcarsCat::Maint: return "Maintenance";
    case AcarsCat::Fault: return "Fault";
    default: return "";
    }
}

AcarsCat acarsClassify(const std::string& label, const std::string& text) {
    AcarsCat base = AcarsCat::Free;
    bool known = false;
    for (const auto& lc : kLabelCats) {
        if (label != lc.label) continue;
        known = true;
        switch (lc.cat) {
        case 'I': return AcarsCat::Ignore;
        case 'O': base = AcarsCat::Ops; break;
        case 'C': base = AcarsCat::Company; break;
        case 'M': base = AcarsCat::Maint; break;
        default: base = AcarsCat::Free; break;
        }
        break;
    }
    (void)known;
    // a technical block with a label such as "_d" never reaches here; a block with no text is nothing to read
    size_t printable = 0;
    for (unsigned char c : text) if (c > 32) printable++;
    if (printable < 4) return AcarsCat::Ignore;
    if (binaryish(text)) return AcarsCat::Ignore;
    const bool ok = readable(text);
    const std::string up = upper(text);
    if (hasWord(up, "FAULT") || hasWord(up, "FAIL") || hasWord(up, "WARN") || hasWord(up, "ECAM") || hasWord(up, "CAUTION")) return ok || base != AcarsCat::Free ? AcarsCat::Fault : AcarsCat::Ignore;
    if (hasWord(up, "CFDS") || hasWord(up, "MAINT") || hasWord(up, "MEL ") || hasWord(up, "DEFECT") || hasWord(up, "PFR") || hasWord(up, "ATA "))
        return ok || base != AcarsCat::Free ? AcarsCat::Maint : AcarsCat::Ignore;
    if (base == AcarsCat::Ops || base == AcarsCat::Company || base == AcarsCat::Maint) {
        // table labels: the text only has to be there, their reports are formatted data
        if (hasWord(up, "CAPT") || hasWord(up, "CREW") || hasWord(up, "CABIN") || hasWord(up, "PURSER")) return AcarsCat::Crew;
        return base;
    }
    if (!ok) return AcarsCat::Ignore;
    if (hasWord(up, "CAPT") || hasWord(up, "CREW") || hasWord(up, "CABIN") || hasWord(up, "PURSER") || hasWord(up, "PAX ")) return AcarsCat::Crew;
    return AcarsCat::Free;
}

std::string acarsRedact(const std::string& s) {
    std::string out;
    const size_t n = s.size();
    size_t i = 0;
    auto isEmailChar = [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-'; };
    while (i < n) {
        const unsigned char c = (unsigned char)s[i];
        // e-mail: a run of local-part characters, '@', a domain with a dot
        if (isEmailChar(c)) {
            size_t j = i;
            while (j < n && isEmailChar((unsigned char)s[j])) j++;
            if (j < n && s[j] == '@') {
                size_t k = j + 1;
                bool dot = false;
                while (k < n && (std::isalnum((unsigned char)s[k]) || s[k] == '.' || s[k] == '-')) { if (s[k] == '.' && k + 1 < n && std::isalpha((unsigned char)s[k + 1])) dot = true; k++; }
                while (k > j + 1 && (s[k - 1] == '.' || s[k - 1] == '-')) k--;
                if (dot && k > j + 3) {
                    // drop what the local part already put in `out`: it started at i, so nothing was appended yet
                    out += "[email]";
                    i = k;
                    continue;
                }
            }
        }
        // phone: optional '+', digits grouped by one separator each
        if (std::isdigit(c) || c == '+' || c == '(') {
            size_t j = i;
            const bool plus = c == '+';
            if (plus || c == '(') j++;
            size_t digits = 0, groups = 0, k = j;
            int prevDigits = 0;
            bool allFour = true;          // groups of exactly four digits: a list of times ("0540 0545 0550"), not a number
            size_t lastDigitEnd = i;
            while (k < n) {
                const unsigned char d = (unsigned char)s[k];
                if (std::isdigit(d)) { digits++; prevDigits++; k++; lastDigitEnd = k; }
                else if ((d == ' ' || d == '-' || d == '.' || d == '(' || d == ')' || d == '/') && prevDigits > 0 && k + 1 < n
                         && (std::isdigit((unsigned char)s[k + 1]) || s[k + 1] == '(' || s[k + 1] == ' ')) { groups++; if (prevDigits != 4 || d != ' ') allFour = false; prevDigits = 0; k++; }
                else if (d == ')' && prevDigits > 0) { k++; }
                else break;
            }
            if (prevDigits != 4) allFour = false;
            const bool prefixed = plus || (j + 1 < n && s[i] == '0' && s[i + 1] == '0');
            const bool before = i > 0 && isAlnum((unsigned char)s[i - 1]);       // inside a word or a longer code: not a number
            const bool after = lastDigitEnd < n && std::isalpha((unsigned char)s[lastDigitEnd]);
            bool phone = false;
            if (!before && !after) {
                if (prefixed && digits >= 8) phone = true;
                else if (groups >= 2 && digits >= 9 && digits <= 15 && !allFour) phone = true;
            }
            if (phone) {
                out += "[number]";
                i = lastDigitEnd;
                continue;
            }
        }
        out += (char)c;
        i++;
    }
    return out;
}

std::string acarsNormReg(const std::string& reg) {
    std::string o;
    for (unsigned char c : reg) if (std::isalnum(c)) o += (char)std::toupper(c);
    return o;
}

std::string acarsNormFlight(const std::string& flight) {
    std::string o;
    for (unsigned char c : flight) if (std::isalnum(c)) o += (char)std::toupper(c);
    size_t p = 0;
    while (p < o.size() && std::isalpha((unsigned char)o[p])) p++;
    size_t q = p;
    while (q + 1 < o.size() && o[q] == '0') q++;
    o.erase(p, q - p);
    return o;
}

std::string AcarsLog::keyHex(uint32_t icao) {
    char b[12];
    snprintf(b, sizeof b, "H:%06X", icao & 0xFFFFFFu);
    return b;
}

bool AcarsLog::add(const AcarsIdent& id, AcarsLogEntry e, bool keepIgnored) {
    if (!keepIgnored && !acarsInteresting(e.cat)) return false;
    if (e.text.size() > kAcarsLogTextMax) e.text.resize(kAcarsLogTextMax);
    const std::string reg = acarsNormReg(id.reg);
    const std::string fl = acarsNormFlight(id.flight.empty() ? e.flight : id.flight);
    uint32_t icao = id.icao & 0xFFFFFFu;
    if (icao && !reg.empty()) {
        if (alias_.size() > 4000) alias_.clear();
        alias_[reg] = icao;
    } else if (!icao && !reg.empty()) {
        auto it = alias_.find(reg);
        if (it != alias_.end()) icao = it->second;
    }
    std::string key;
    if (icao) key = keyHex(icao);
    else if (!reg.empty()) key = "R:" + reg;
    else if (!fl.empty()) key = "F:" + fl;
    else return false;                      // nobody to attach it to
    if (icao && !reg.empty()) {             // move what was stored under the bare registration
        auto it = buckets_.find("R:" + reg);
        if (it != buckets_.end()) {
            Bucket& dst = buckets_[key];
            for (auto& m : it->second.v) dst.v.push_back(std::move(m));
            std::stable_sort(dst.v.begin(), dst.v.end(), [](const AcarsLogEntry& a, const AcarsLogEntry& b) { return a.wall < b.wall; });
            dst.last = std::max(dst.last, it->second.last);
            buckets_.erase("R:" + reg);
        }
    }
    Bucket& b = buckets_[key];
    for (auto it = b.v.rbegin(); it != b.v.rend() && e.wall - it->wall <= 30; ++it)
        if (it->label == e.label && it->text == e.text) return false;
    b.last = std::max(b.last, e.wall);
    b.v.push_back(std::move(e));
    while (b.v.size() > kAcarsLogPerAircraft) b.v.pop_front();
    if (buckets_.size() > kAcarsLogMaxAircraft) {
        auto oldest = buckets_.begin();
        for (auto it = buckets_.begin(); it != buckets_.end(); ++it) if (it->second.last < oldest->second.last) oldest = it;
        buckets_.erase(oldest);
    }
    return true;
}

std::vector<AcarsLogEntry> AcarsLog::messagesFor(const AcarsAircraftKey& k, int64_t nowWall) const {
    std::vector<const Bucket*> src;
    auto take = [&](const std::string& key) { auto it = buckets_.find(key); if (it != buckets_.end()) src.push_back(&it->second); };
    if (k.icao) take(keyHex(k.icao));
    const std::string reg = acarsNormReg(k.reg);
    if (!reg.empty()) take("R:" + reg);
    const std::string fl = acarsNormFlight(k.callsign);
    if (!fl.empty() && k.callsignHolders == 1) take("F:" + fl);
    std::vector<AcarsLogEntry> out;
    for (const Bucket* b : src)
        for (const auto& m : b->v) if (nowWall - m.wall <= kAcarsLogMaxAgeSec) out.push_back(m);
    std::stable_sort(out.begin(), out.end(), [](const AcarsLogEntry& a, const AcarsLogEntry& b) { return a.wall > b.wall; });
    if (out.size() > kAcarsLogPerAircraft) out.resize(kAcarsLogPerAircraft);
    return out;
}

void AcarsLog::prune(int64_t nowWall) {
    for (auto it = buckets_.begin(); it != buckets_.end();) {
        auto& v = it->second.v;
        while (!v.empty() && nowWall - v.front().wall > kAcarsLogMaxAgeSec) v.pop_front();
        if (v.empty()) it = buckets_.erase(it); else ++it;
    }
}

} // namespace dect2
