// Finding and calling the user's patched FAAD2 (see hdr_hdc.h and hdr_hdc_abi.h). The same search as the mbelib and codec2 loaders
// (source/native_common.h): DECT2_FAAD_HDC_PATH, the Eye of SaurCom codecs folders, the program's folder, the system's places.
#include "dect2/hdr/hdr_hdc.h"
#include "hdr_hdc_abi.h"
#include "../source/native_common.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

// The file names Eye of SaurCom accepts: libfaad_hdc is what the patch's CMake target is called (nrsc5's own build names it so); libfaad2-hdc is the
// distribution style name.
#define DECT2_FAADHDC_STEM "faad_hdc"
#define DECT2_FAADHDC_SO {".2", ".0"}
#define DECT2_FAADHDC_WIN {"libfaad_hdc.dll", "faad_hdc.dll"}
#define DECT2_FAADHDC2_STEM "faad2-hdc"
#define DECT2_FAADHDC2_SO {".2", ".0"}
#define DECT2_FAADHDC2_WIN {"libfaad2-hdc.dll", "faad2-hdc.dll"}

namespace dect2 { namespace hdr {

namespace {

struct Api {
    native::DynLib lib;
    bool ok = false;
    std::string problem;        // why a library that was found cannot be used
    FaadInitHdcFn init = nullptr;
    FaadDecodeFn decode = nullptr;
    FaadCloseFn close = nullptr;
    FaadErrMsgFn errMsg = nullptr;
    FaadVersionFn version = nullptr;
    std::string versionText;
};

// What a frame info that came back from the library has to look like. A layout that differs from the one in hdr_hdc_abi.h puts other fields
// where Eye of SaurCom reads them and shows up here as impossible numbers.
bool plausible(const FaadFrameInfo& i) {
    return i.error < 64 && i.channels <= 64 && i.samplerate <= 192000 && i.samples <= (1u << 20);
}

bool parseVersion(const char* s, int& major, int& minor) {
    return s && sscanf(s, "%d.%d", &major, &minor) == 2;
}

std::vector<std::string> candidates() {
    std::vector<std::string> v = native::codecLibNames(DECT2_FAADHDC_STEM, DECT2_FAADHDC_SO, DECT2_FAADHDC_WIN);
    for (auto& p : native::codecLibNames(DECT2_FAADHDC2_STEM, DECT2_FAADHDC2_SO, DECT2_FAADHDC2_WIN)) if (std::find(v.begin(), v.end(), p) == v.end()) v.push_back(p);
    return v;
}

const Api* scan() {
    Api* r = new Api;   // kept for the life of the program: decoders hold the one they started with
    std::vector<std::string> names;
    if (const char* e = getenv("DECT2_FAAD_HDC_PATH")) if (*e) names.push_back(e);
    if (names.empty()) names = candidates();
    if (!r->lib.open(names)) return r;     // not found: problem stays empty
    native::DynLib& l = r->lib;
    if (!l.get(r->decode, "NeAACDecDecode") || !l.get(r->close, "NeAACDecClose")) { r->problem = "this library is not a FAAD2 (NeAACDecDecode / NeAACDecClose are missing)"; return r; }
    if (!l.opt(r->init, "NeAACDecInitHDC")) { r->problem = "this FAAD2 has no NeAACDecInitHDC: it is the plain FAAD2, not the HDC-patched build"; return r; }
    l.opt(r->errMsg, "NeAACDecGetErrorMessage");
    if (!r->errMsg) { r->problem = "this library is not a FAAD2 (NeAACDecGetErrorMessage is missing)"; return r; }
    // The version: the frame info layout is the same from 2.7 to 2.11. Older or newer (a 3.x that changed it) is refused rather than guessed.
    if (l.opt(r->version, "NeAACDecGetVersion")) {
        const char* id = nullptr; const char* cp = nullptr;
        r->version(&id, &cp);
        int ma = 0, mi = 0;
        if (id) r->versionText = id;
        if (!parseVersion(id, ma, mi)) { r->problem = "cannot read the FAAD2 version (\"" + r->versionText + "\"): not a layout Eye of SaurCom knows"; return r; }
        if (ma != 2 || mi < 7 || mi > 99) { r->problem = "FAAD2 version " + r->versionText + " is not one Eye of SaurCom knows (2.7 to 2.11); build the patch on a 2.10 or 2.11 source"; return r; }
    }
    // The canary: decode one byte of nothing. Any FAAD2 refuses it with an error; the answer has to be inside the ranges of our layout.
    FaadHandle h = nullptr;
    r->init(&h);
    if (!h) { r->problem = "NeAACDecInitHDC gave no decoder"; return r; }
    FaadFrameInfo info;
    memset(&info, 0, sizeof info);
    unsigned char junk[1] = {0};
    r->decode(h, &info, junk, 1);
    r->close(h);
    if (!plausible(info)) { r->problem = "the frame info of this FAAD2 does not have the layout Eye of SaurCom expects (a different FAAD2 version?)"; return r; }
    r->ok = true;
    return r;
}

std::mutex gMu;
std::atomic<const Api*> gApi{nullptr};

const Api& api() {
    if (const Api* a = gApi.load(std::memory_order_acquire)) return *a;
    std::lock_guard<std::mutex> lk(gMu);
    if (!gApi.load()) gApi.store(scan(), std::memory_order_release);
    return *gApi.load();
}
} // namespace

bool hdcAvailable(std::string* path, std::string* problem) {
    const Api& a = api();
    if (path && a.lib.h) *path = a.lib.path;
    if (problem) *problem = a.problem;
    return a.ok;
}

bool hdcRescan() {
    if (api().ok) return true;
    std::lock_guard<std::mutex> lk(gMu);
    gApi.store(scan(), std::memory_order_release);   // the old one stays allocated: a decoder may still hold it
    return gApi.load()->ok;
}

std::vector<std::string> hdcFileNames() {
    std::vector<std::string> v = native::codecFileNames(DECT2_FAADHDC_STEM, DECT2_FAADHDC_SO, DECT2_FAADHDC_WIN);
    for (auto& n : native::codecFileNames(DECT2_FAADHDC2_STEM, DECT2_FAADHDC2_SO, DECT2_FAADHDC2_WIN)) v.push_back(n);
    return v;
}
std::vector<std::string> hdcCandidatePaths() { return candidates(); }

// ------------------------------------------------------------------ one decoder state

struct HdcDecoder::Impl {
    const Api* a = nullptr;
    FaadHandle h = nullptr;
    void closeState() { if (h && a) a->close(h); h = nullptr; }
};

HdcDecoder::HdcDecoder() : p_(new Impl) {
    const Api& a = api();
    if (a.ok) p_->a = &a;
}
HdcDecoder::~HdcDecoder() { p_->closeState(); }
bool HdcDecoder::ok() const { return p_->a != nullptr; }
void HdcDecoder::reset() { p_->closeState(); }

int HdcDecoder::decode(const uint8_t* pkt, size_t n, std::vector<int16_t>& pcm) {
    pcm.clear();
    if (!p_->a || !n) return -1;
    if (!p_->h) {
        p_->a->init(&p_->h);
        if (!p_->h) return -1;
    }
    FaadFrameInfo info;
    memset(&info, 0, sizeof info);
    // the library may modify nothing it is given, but its prototype takes a non-const pointer
    void* out = p_->a->decode(p_->h, &info, const_cast<unsigned char*>(pkt), (unsigned long)n);
    if (!plausible(info) || info.error != 0 || !out || info.samples == 0) return -1;
    const unsigned ch = info.channels;
    if ((ch != 1 && ch != 2) || info.samplerate != 44100) return -1;     // HDC is 44.1 kHz stereo; anything else is not decoded as HDC
    const size_t frames = info.samples / ch;
    if (frames == 0 || frames > 8192) return -1;
    pcm.resize(frames * 2);
    const int16_t* s = static_cast<const int16_t*>(out);    // the library's default output: 16-bit interleaved
    if (ch == 2) memcpy(pcm.data(), s, frames * 2 * sizeof(int16_t));
    else for (size_t i = 0; i < frames; i++) pcm[2 * i] = pcm[2 * i + 1] = s[i];
    return (int)frames;
}

}} // namespace dect2::hdr
