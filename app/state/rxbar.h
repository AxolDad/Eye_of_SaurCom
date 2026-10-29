// Pure helpers of the receiver strip (receivers.cpp): label shortening, chip widths, Ctrl+digit mapping, tab scroll.
#pragma once
#include <algorithm>
#include <cstdio>
#include <string>

namespace rxbar {
// the label "1 · ADS-B · 1090.000 MHz"; the mode name is what gives way first (see ellipsize)
inline std::string label(int rx, const char* mode, double mhz) {
    char b[160];
    snprintf(b, sizeof b, "%d \xC2\xB7 %s \xC2\xB7 %.3f MHz", rx, mode, mhz);
    return b;
}
// cut s to at most maxChars UTF-8 characters, ending in "..." (an ellipsis character) when cut; never splits a multi-byte character
inline std::string ellipsize(const std::string& s, int maxChars) {
    if (maxChars <= 0) return "";
    int n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    if (n <= maxChars) return s;
    if (maxChars == 1) return "\xE2\x80\xA6";
    int keep = maxChars - 1, seen = 0;
    size_t i = 0;
    for (; i < s.size(); i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) { if (seen == keep) break; seen++; }
    }
    return s.substr(0, i) + "\xE2\x80\xA6";
}
// every digit as a 0: measuring this instead of the real label keeps a chip's width steady while the frequency changes (the default
// font, Inter, has proportional digits; 0 is among its widest)
inline std::string zeroDigits(std::string s) { for (auto& c : s) if (c >= '0' && c <= '9') c = '0'; return s; }
// number of UTF-8 characters
inline int utf8Chars(const std::string& s) { int n = 0; for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++; return n; }
// "2 · NESDR SMArt v5 · General receiver · 145.500 MHz"; an empty radio or mode is left out
inline std::string label(int rx, const std::string& radio, const std::string& mode, double mhz) {
    char f[32];
    snprintf(f, sizeof f, "%.3f MHz", mhz);
    std::string s = std::to_string(rx);
    if (!radio.empty()) s += " \xC2\xB7 " + radio;
    if (!mode.empty()) s += " \xC2\xB7 " + mode;
    return s + " \xC2\xB7 " + f;
}
// The longest form of that label whose measure(text) fits room. What gives way, in order: the radio name (down to 8 characters with
// an ellipsis), then the mode (down to 6), then the radio name goes, then the mode; the number and the frequency always stay.
template <class Measure>
std::string fitLabel(int rx, const std::string& radio, const std::string& mode, double mhz, float room, Measure measure) {
    auto fits = [&](const std::string& t) { return measure(t) <= room; };
    std::string t = label(rx, radio, mode, mhz);
    if (fits(t)) return t;
    const int rn = utf8Chars(radio), mn = utf8Chars(mode);
    for (int n = rn - 1; n >= 8; n--) if (fits(t = label(rx, ellipsize(radio, n), mode, mhz))) return t;
    const std::string r8 = rn > 8 ? ellipsize(radio, 8) : radio;
    for (int n = mn - 1; n >= 6; n--) if (fits(t = label(rx, r8, ellipsize(mode, n), mhz))) return t;
    for (int n = mn; n >= 4; n--) if (fits(t = label(rx, "", ellipsize(mode, n), mhz))) return t;
    return label(rx, "", "", mhz);
}
// A frequency for a new receiver that the radio can tune: the mode's own default when the radio covers it (or its range is not
// known: max <= 0), else 100 MHz (FM broadcast) when it covers that, else the default moved just inside the radio's range.
inline double tuneInRange(double preferMhz, double radioMinMhz, double radioMaxMhz) {
    if (!(radioMaxMhz > 0) || radioMaxMhz <= radioMinMhz) return preferMhz;
    if (preferMhz >= radioMinMhz && preferMhz <= radioMaxMhz) return preferMhz;
    if (100.0 >= radioMinMhz && 100.0 <= radioMaxMhz) return 100.0;
    const double margin = (radioMaxMhz - radioMinMhz) * 0.01;
    return std::clamp(preferMhz, radioMinMhz + margin, radioMaxMhz - margin);
}
// width of a chip given the text width wanted: clamped to [minW, maxW]
inline float chipWidth(float textW, float extras, float minW, float maxW) { return std::clamp(textW + extras, minW, maxW); }
// Ctrl+1..9 -> index into the receiver list (0-based), -1 when the digit has no receiver
inline int indexForDigit(int digit, int count) { return digit >= 1 && digit <= 9 && digit <= count ? digit - 1 : -1; }
// scroll offset that brings the chip [x0, x1] fully into a viewport of width vw, starting from scroll
inline float scrollToShow(float scroll, float x0, float x1, float vw) {
    if (x0 < scroll) return x0;
    if (x1 > scroll + vw) return x1 - vw;
    return scroll;
}
}  // namespace rxbar
