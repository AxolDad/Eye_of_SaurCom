#include "dect2/common/u8file.h"
#include "dect2/source/iq_file.h"
#include "dect2/files/files_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>
#include "dect2/common/str.h"
#include "dect2/common/endian.h"

namespace dect2 {
namespace {

std::string lower(std::string x) { return dect2::str::lower(std::move(x)); }
bool endsWith(const std::string& s, const char* e) { size_t n = strlen(e); return s.size() >= n && lower(s.substr(s.size() - n)) == e; }
std::filesystem::path fsPath(const std::string& p) { return dect2::u8file(p); }

bool readAll(const std::string& path, std::string& out) {
    std::ifstream f(fsPath(path), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf(); out = ss.str();
    return true;
}

size_t bytesPerSample(FileFormat f) { return f == FileFormat::CF32 ? 8 : (f == FileFormat::CI16 || f == FileFormat::CU16) ? 4 : 2; }

bool probeSigmf(const std::string& metaPath, const std::string& dataPathIn, IqFileInfo& out, std::string& err) {
    std::string txt;
    if (!readAll(metaPath, txt)) { err = "cannot read " + metaPath; return false; }
    // the meta text is read by core/src/files/files_proto.cpp (C++) or the Rust crate zorcom_files
    const filesp::SigmfMeta m = filesp::sigmfParse(txt);
    if (m.status == 1) { err = "not a valid SigMF meta file: " + metaPath; return false; }
    if (m.status == 2) { err = "SigMF meta has no \"global\" section"; return false; }
    if (m.status == 3) { err = "SigMF meta has no core:datatype"; return false; }
    if (m.status == 4) { err = "SigMF datatype \"" + m.datatype + "\" is not supported (complex cf32, ci16, cu16, ci8, cu8 are)"; return false; }
    IqFileInfo r;
    r.format = static_cast<FileFormat>(m.format);   // the same order: CS8, CU8, CF32, CI16, CU16
    r.bigEndian = m.bigEndian;
    r.sampleRate = m.sampleRate;
    r.description = m.description;
    r.centerHz = m.centerHz;
    r.annotations = m.annotations;
    const uint64_t sampleStart = m.sampleStart;
    std::string data = dataPathIn;
    if (data.empty()) {
        const std::string base = metaPath.substr(0, metaPath.size() - strlen(".sigmf-meta"));
        const std::filesystem::path metaDir = fsPath(metaPath).parent_path();
        if (!m.dataset.empty()) {
            std::filesystem::path dp = fsPath(m.dataset);
            if (!dp.is_absolute()) dp = metaDir / dp;
            if (std::filesystem::exists(dp)) data = dp.string();
        }
        if (data.empty())
            for (const char* ext : {".sigmf-data", ".SIGMF-DATA"}) if (std::filesystem::exists(fsPath(base + ext))) { data = base + ext; break; }
        if (data.empty()) { err = "no IQ data file for " + metaPath + " (expected .sigmf-data or global core:dataset)"; return false; }
    }
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(fsPath(data), ec);
    if (ec) { err = "cannot read " + data; return false; }
    const uint64_t bps = bytesPerSample(r.format);
    const uint64_t hdr = m.headerBytes;
    const uint64_t trail = m.trailingBytes;
    r.offsetBytes = hdr + sampleStart * bps;
    if (r.offsetBytes > size) { err = "SigMF sample_start is past the end of the data file"; return false; }
    if (trail && trail < size) r.endBytes = std::max(size - trail, r.offsetBytes);
    r.dataPath = data;
    out = r;
    return true;
}

uint32_t rd32(const uint8_t* p) { return (uint32_t)dect2::endian::loadLe32(p); }

bool probeWav(const std::string& path, IqFileInfo& out, std::string& err) {
    std::ifstream f(fsPath(path), std::ios::binary);
    if (!f) { err = "cannot read " + path; return false; }
    uint8_t h[12];
    if (!f.read((char*)h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { err = "not a WAV file: " + path; return false; }
    uint16_t tag = 0, ch = 0, bits = 0; uint32_t rate = 0;
    bool haveFmt = false;
    IqFileInfo r;
    uint8_t ck[8];
    while (f.read((char*)ck, 8)) {
        const uint32_t len = rd32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            std::vector<uint8_t> b(std::max<uint32_t>(len, 16));
            if (!f.read((char*)b.data(), len)) break;
            const filesp::WavFmt w = filesp::wavFmt(b.data(), b.size(), len);   // the fields of the chunk: files_proto.cpp (C++) or the Rust crate zorcom_files
            tag = (uint16_t)w.tag; ch = (uint16_t)w.channels; rate = w.rate; bits = (uint16_t)w.bits;
            haveFmt = true;
            if (len & 1) f.seekg(1, std::ios::cur);
        } else if (!memcmp(ck, "data", 4)) {
            if (!haveFmt) { err = "WAV data before its format"; return false; }
            int fmtCode = 0;
            const int cls = filesp::wavClassify(filesp::WavFmt{tag, ch, rate, bits}, fmtCode);
            if (cls == 1) { err = "a WAV needs two channels (I and Q) to play as IQ, this one has " + std::to_string(ch); return false; }
            if (cls != 0) { err = "WAV sample format not supported (16-bit, 8-bit or 32-bit float are)"; return false; }
            r.format = static_cast<FileFormat>(fmtCode);
            r.sampleRate = rate;
            r.offsetBytes = (uint64_t)f.tellg();
            std::error_code ec;
            const uint64_t size = std::filesystem::file_size(fsPath(path), ec);
            r.endBytes = filesp::wavDataEnd(r.offsetBytes, len, size);
            r.dataPath = path;
            // SDR programs put the centre frequency in the name: "..._100.000MHz_..."
            r.centerHz = filesp::wavCenterFromName(fsPath(path).filename().string());
            out = r;
            return true;
        } else f.seekg((std::streamoff)len + (len & 1), std::ios::cur);
        if (!f) break;
    }
    err = "no audio data in " + path;
    return false;
}

} // namespace

bool probeIqFile(const std::string& path, IqFileInfo& out, std::string& err) {
    err.clear();
    if (endsWith(path, ".sigmf-meta")) return probeSigmf(path, "", out, err);
    if (endsWith(path, ".sigmf-data")) {
        const std::string meta = path.substr(0, path.size() - strlen(".sigmf-data")) + ".sigmf-meta";
        if (!std::filesystem::exists(fsPath(meta))) { err = "no .sigmf-meta file beside " + path; return false; }
        return probeSigmf(meta, path, out, err);
    }
    if (endsWith(path, ".wav")) return probeWav(path, out, err);
    return false;
}

std::vector<RecordingListing> listRecordingFiles(const std::string& dir) {
    std::vector<RecordingListing> out;
    std::error_code ec;
    const std::filesystem::path d = fsPath(dir);
    if (!std::filesystem::is_directory(d, ec)) return out;
    static const char* const kRaw[] = {".cs8", ".cu8", ".cf32", ".ci16", ".cu16", ".wav"};
    std::vector<std::string> names;
    for (std::filesystem::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) names.push_back(it->path().filename().string());
    // a recording with a .sigmf-meta is listed once, by the meta; its data (the hard link, or the .sigmf-data) and the raw copy are skipped
    std::map<std::string, bool> metaBase;
    for (const std::string& n : names) if (endsWith(n, ".sigmf-meta")) metaBase[n.substr(0, n.size() - strlen(".sigmf-meta"))] = true;
    for (const std::string& n : names) {
        const bool meta = endsWith(n, ".sigmf-meta");
        if (!meta) {
            if (endsWith(n, ".sigmf-data")) continue;   // listed by its meta, or not a recording without one
            bool raw = false;
            for (const char* e : kRaw) if (endsWith(n, e)) raw = true;
            if (!raw) continue;
            const std::string base = n.substr(0, n.find_last_of('.'));
            if (metaBase.count(base)) continue;
        }
        const std::string path = (d / n).string();
        RecordingListing r;
        r.fileName = n; r.path = path; r.sigmf = meta;
        IqFileInfo info; std::string err;
        std::string dataPath = path;
        if (probeIqFile(path, info, err)) {
            r.format = info.format; r.sampleRate = info.sampleRate; r.centerHz = info.centerHz; r.mode = info.description;
            dataPath = info.dataPath;
            const uint64_t size = std::filesystem::file_size(fsPath(dataPath), ec);
            ec.clear();
            const uint64_t end = info.endBytes ? info.endBytes : size;
            if (end > info.offsetBytes && r.sampleRate > 0) r.seconds = (double)((end - info.offsetBytes) / bytesPerSample(r.format)) / r.sampleRate;
        } else {
            if (!err.empty() || meta) continue;   // a WAV or SigMF that cannot be played: not listed
            r.format = guessFormat(path);          // a raw file: the rate and format its name says
            r.sampleRate = guessSampleRate(path);
            const uint64_t size = std::filesystem::file_size(fsPath(path), ec);
            ec.clear();
            if (r.sampleRate > 0) r.seconds = (double)(size / bytesPerSample(r.format)) / r.sampleRate;
        }
        const auto ft = std::filesystem::last_write_time(fsPath(path), ec);
        if (!ec) {
            const auto sys = std::chrono::time_point_cast<std::chrono::seconds>(ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            r.modified = (int64_t)sys.time_since_epoch().count();
        }
        ec.clear();
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const RecordingListing& a, const RecordingListing& b) { return a.modified > b.modified; });
    return out;
}

const char* sigmfDatatype(FileFormat fmt, bool be) {
    switch (fmt) {
    case FileFormat::CF32: return be ? "cf32_be" : "cf32_le";
    case FileFormat::CI16: return be ? "ci16_be" : "ci16_le";
    case FileFormat::CU16: return be ? "cu16_be" : "cu16_le";
    case FileFormat::CU8: return "cu8";
    default: return "ci8";
    }
}

bool writeSigmfMeta(const std::string& metaPath, FileFormat fmt, bool be, double rate, double center, uint64_t sampleStart,
                    const std::string& description, std::string& err) {
    std::ofstream f(fsPath(metaPath), std::ios::binary);
    if (!f) { err = "cannot create " + metaPath; return false; }
    const std::string b = filesp::sigmfToolMeta(sigmfDatatype(fmt, be), rate, center, sampleStart, description);   // the text: files_proto.cpp (C++) or the Rust crate zorcom_files
    f << b;
    if (!f) { err = "writing failed: " + metaPath; return false; }
    return true;
}

} // namespace dect2
