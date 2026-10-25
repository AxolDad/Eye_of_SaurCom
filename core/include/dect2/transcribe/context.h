// What is on the air right now, offered to the speech engine as words it should expect.
//
// A speech engine guesses from what it has heard before, and it has never heard of the aircraft overhead this minute, the talkgroup that just
// got a channel, or the airport down the road. It will turn "Ryanair three four alpha" into "Ryan air 34 alpha" or worse. Giving it the names
// that are actually in the air costs nothing and fixes most of that: whisper.cpp takes an initial prompt, and a word in the prompt is a word
// the decoder leans towards.
//
// The store is a set of named scopes, each holding terms with a weight and when they were last seen. The transcriber asks for the prompt of a
// source and gets the terms of every scope that applies to it, most recent and heaviest first, trimmed to a character budget - because the
// prompt is capped (whisper reads 224 tokens of it) and an unbudgeted list would push out the domain phraseology that comes before it.
//
// Nothing here knows where the terms came from: the radio side fills the scopes, this only ranks and trims. That keeps the ranking testable
// without a radio, and lets anything at all be a feeder.
#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dect2 {

struct SourceInfo;

// One thing worth expecting. `spoken` is written the way it should come out of the engine ("Lufthansa 1492", "Medic 7", "runway two five
// left"); nothing here reformats it.
struct ContextTerm {
    std::string spoken;
    float weight = 1;        // how much it is worth biasing: an aircraft being talked to now beats one at the edge of range
    int64_t seenMs = 0;      // when it was last seen, Unix ms; 0 means "no time known", which ranks as oldest
};

// The scopes a source draws from, in the order they are asked for. A term in a more specific scope wins over the same term in a wider one.
// "all"            every source (the user's own words, watchlists)
// "<domain>"       the source's domain: "aviation", "marine", "general"
// "freq:<hz>"      the exact frequency, as scopeForFreq() spells it
// "chan:<name>"    a named channel or talkgroup, as scopeForChannel() spells it
std::string scopeForFreq(double freqHz);
std::string scopeForChannel(const std::string& name);

class ContextStore {
public:
    static ContextStore& global();

    // Replaces a scope's terms. An empty list removes the scope. Cheap enough to call every second with a few hundred terms.
    void setTerms(const std::string& scope, std::vector<ContextTerm> terms);
    // Adds or refreshes one term without disturbing the rest of its scope.
    void addTerm(const std::string& scope, const ContextTerm& term);
    void clear();
    void clearScope(const std::string& scope);

    // The terms a source should be biased towards, best first: heaviest and most recently seen, de-duplicated across scopes, and cut off at
    // `maxChars` of joined length. Terms older than `maxAgeMs` are left out (0: no age limit).
    std::vector<ContextTerm> termsFor(const SourceInfo& src, size_t maxChars, int64_t nowMs, int64_t maxAgeMs = 0) const;
    // The same, joined into the sentence that goes in the prompt: "Expect: Lufthansa 1492, Medic 7, runway two five left."
    // "" when there is nothing to say, so the caller can leave the prompt alone.
    std::string promptFor(const SourceInfo& src, size_t maxChars, int64_t nowMs, int64_t maxAgeMs = 0) const;

    size_t scopeCount() const;
    size_t termCount() const;
    std::vector<std::string> scopes() const;   // for the UI: which scopes are filled

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::vector<ContextTerm>> scopes_;
};

// The scopes that apply to a source, widest first. Public so the UI can show where a term came from, and for the tests.
std::vector<std::string> scopesFor(const SourceInfo& src);

}  // namespace dect2
