#pragma once
// IQ recordings with a description: SigMF (.sigmf-meta + .sigmf-data) and two-channel WAV. probeIqFile() reads the rate, the centre
// frequency, the sample format and where the samples start, so the file plays in any mode without typing them in.
#include "source.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct IqFileInfo {
    std::string dataPath;           // the file that holds the samples
    FileFormat format = FileFormat::CS8;
    bool bigEndian = false;
    double sampleRate = 0;          // Hz, 0 = the file does not say
    double centerHz = 0;            // Hz, 0 = the file does not say
    uint64_t offsetBytes = 0;       // first sample (SigMF header_bytes + captures[0].sample_start, or the WAV data chunk)
    uint64_t endBytes = 0;          // 0 = until the end of the file
    int annotations = 0;            // SigMF annotation count (informational)
    std::string description;
};

// .sigmf-meta, .sigmf-data (the meta beside it is found), .wav. Returns false with err set when the file cannot be read or is a
// kind Eye of SaurCom cannot play (real-valued samples, 64-bit floats ...). Other extensions return false with an empty err.
bool probeIqFile(const std::string& path, IqFileInfo& out, std::string& err);

// One recording in a folder, as the recording library lists it. Only the sidecar (or the file header) is read, never the samples.
struct RecordingListing {
    std::string fileName;           // the name in the folder: the .sigmf-meta, or the raw / WAV file when there is no sidecar
    std::string path;               // what opening the recording takes
    FileFormat format = FileFormat::CS8;
    double sampleRate = 0;          // Hz, 0 = unknown
    double centerHz = 0;            // Hz, 0 = unknown (no sidecar, and the name does not say)
    double seconds = 0;             // length from the data size and the rate, 0 = unknown
    int64_t modified = 0;           // last write, seconds since the Unix epoch
    std::string mode;               // the sidecar's description (the recorder writes the mode id there); "" when none
    bool sigmf = false;
};
// The recordings in a folder (not recursive), newest first. A recording with a .sigmf-meta is listed once, by its meta.
std::vector<RecordingListing> listRecordingFiles(const std::string& dir);

// SigMF writer used by tests and tools: writes the `.sigmf-meta` that describes a `.sigmf-data` file.
bool writeSigmfMeta(const std::string& metaPath, FileFormat fmt, bool bigEndian, double rateHz, double centerHz,
                    uint64_t sampleStart, const std::string& description, std::string& err);
const char* sigmfDatatype(FileFormat fmt, bool bigEndian);

} // namespace dect2
