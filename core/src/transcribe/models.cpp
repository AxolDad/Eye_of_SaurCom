#include "dect2/common/thread_name.h"
#include "dect2/transcribe/models.h"
#include "dect2/common/u8file.h"
#include "dect2/engine/updater.h"
#include <algorithm>
#include <chrono>

namespace dect2 {

namespace {
const char* kBase = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/";
}

const std::vector<SpeechModel>& speechModels() {
    // Sizes and digests from the model repository's file list (the LFS object ids), checked against a download of tiny.en.
    static const std::vector<SpeechModel> m = {
        {"tiny.en", "ggml-tiny.en.bin", 77704715ull, "921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f", false, 75},
        {"base.en", "ggml-base.en.bin", 147964211ull, "a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002", false, 142},
        {"small.en", "ggml-small.en.bin", 487614201ull, "c6138d6d58ecc8322097e0f987c32f1be8bb0a18532a3f88f734d1bbf9c41e5d", false, 466},
        {"tiny", "ggml-tiny.bin", 77691713ull, "be07e048e1e599ad46341c8d2a135645097a538221678b7acdd1b1919c6e1b21", true, 75},
        {"base", "ggml-base.bin", 147951465ull, "60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe", true, 142},
        {"small", "ggml-small.bin", 487601967ull, "1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b", true, 466},
    };
    return m;
}

const SpeechModel* findSpeechModel(const std::string& id) {
    for (const auto& m : speechModels()) if (id == m.id) return &m;
    return nullptr;
}

std::string speechModelUrl(const SpeechModel& m) { return std::string(kBase) + m.file; }
std::string speechModelPath(const std::string& dir, const SpeechModel& m) { return dir + "/" + m.file; }

bool speechModelInstalled(const std::string& dir, const SpeechModel& m) {
    std::error_code ec;
    const auto p = u8file(speechModelPath(dir, m));
    return std::filesystem::is_regular_file(p, ec) && std::filesystem::file_size(p, ec) == m.bytes;
}

bool speechModelEnglishOnly(const std::string& fileName) { return fileName.find(".en") != std::string::npos; }

std::vector<std::string> userSpeechModels(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(u8file(dir), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string n = it->path().filename().string();
        if (n.size() < 5 || n.compare(n.size() - 4, 4, ".bin") != 0) continue;
        bool known = false;
        for (const auto& m : speechModels()) known |= n == m.file;
        if (!known) out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    return out;
}

ModelDownloader::~ModelDownloader() {
    cancel_ = true;
    if (th_.joinable()) th_.join();
}

std::string ModelDownloader::error() const { std::lock_guard<std::mutex> lk(mu_); return err_; }
std::string ModelDownloader::modelId() const { std::lock_guard<std::mutex> lk(mu_); return id_; }

bool ModelDownloader::start(const SpeechModel& m, const std::string& dir) {
    const State s = state_.load();
    if (s == State::Downloading || s == State::Verifying) return false;
    if (th_.joinable()) th_.join();
    cancel_ = false;
    done_ = 0;
    total_ = m.bytes;
    { std::lock_guard<std::mutex> lk(mu_); err_.clear(); id_ = m.id; }
    state_ = State::Downloading;
    th_ = dect2::namedThread("zorcom-models", [this, m, dir] { run(m, dir); });
    return true;
}

void ModelDownloader::cancel() { cancel_ = true; }

void ModelDownloader::run(SpeechModel m, std::string dir) {
    auto fail = [&](const std::string& e) { { std::lock_guard<std::mutex> lk(mu_); err_ = e; } state_ = State::Failed; };
    std::error_code ec;
    std::filesystem::create_directories(u8file(dir), ec);
    const std::string final = speechModelPath(dir, m), part = final + ".part";
    std::filesystem::remove(u8file(part), ec);
    const std::string url = base_.empty() ? speechModelUrl(m) : base_ + m.file;
    const int rc = curlDownloadFile(url, part, m.bytes + (m.bytes >> 6), 3 * 3600, cancel_, [&] {
        std::error_code e2;
        const auto sz = std::filesystem::file_size(u8file(part), e2);
        if (!e2) done_ = sz;
    });
    if (rc != 0) { std::filesystem::remove(u8file(part), ec); fail(curlDownloadError(rc)); return; }
    state_ = State::Verifying;
    const auto sz = std::filesystem::file_size(u8file(part), ec);
    if (ec || sz != m.bytes) { std::filesystem::remove(u8file(part), ec); fail("the downloaded file has the wrong size: not used"); return; }
    std::string why;
    const std::string h = sha256File(part, &why);
    if (h != m.sha256) { std::filesystem::remove(u8file(part), ec); fail("the SHA-256 of the downloaded file does not match the expected one: deleted, not used"); return; }
    std::filesystem::remove(u8file(final), ec);
    std::filesystem::rename(u8file(part), u8file(final), ec);
    if (ec) { fail("cannot move the model into place: " + ec.message()); return; }
    done_ = m.bytes;
    state_ = State::Done;
}

}  // namespace dect2
