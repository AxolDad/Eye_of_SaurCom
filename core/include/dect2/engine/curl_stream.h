// Runs curl (the system's: /usr/bin/curl, System32\curl.exe on Windows 10 1803 and later; no shell, no console window) and hands its
// output to onData as it arrives, for a long download such as an internet radio stream. Returns when curl ends or cancel is set:
// curl's exit code, -1 when cancelled, -2 when curl could not be started.
#pragma once
#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace dect2 {
int curlStream(const std::vector<std::string>& args, const std::function<void(const char*, size_t)>& onData, const std::atomic<bool>& cancel);
}
