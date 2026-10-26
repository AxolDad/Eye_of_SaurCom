// Eye of SaurCom's replacement for nfc-laboratory's rt/Logger.cpp (which writes its own log file through pthread and sys/time, neither of which
// Eye of SaurCom's Windows build has). Same interface (rt/Logger.h): a logger per name with a level, {} formatting through rt::Format. Messages at
// WARN or above go to the sink Eye of SaurCom installs (nfc_lab_log.h) or, without one, to stderr; INFO and below are dropped unless a level is set.
#include <rt/Format.h>
#include <rt/Logger.h>
#include "nfc_lab_log.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>

namespace rt {

namespace {
std::function<void(int, const std::string&, const std::string&)>& sinkRef() {
    static std::function<void(int, const std::string&, const std::string&)> s;
    return s;
}
int& rootLevelRef() {
    static int l = Logger::WARN_LEVEL;
    return l;
}
int levelOf(const std::string& name) {
    static const char* const tags[] = {"NONE", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"};
    std::string u = name;
    std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c) { return (char)toupper(c); });
    for (int i = 0; i < 6; i++) if (u == tags[i]) return i;
    return Logger::WARN_LEVEL;
}
} // namespace

Logger::Logger(std::string n, int l) : level(l), name(std::move(n)) {}

void Logger::trace(const std::string& format, std::vector<Variant> params) const { print(TRACE_LEVEL, format, std::move(params)); }
void Logger::debug(const std::string& format, std::vector<Variant> params) const { print(DEBUG_LEVEL, format, std::move(params)); }
void Logger::info(const std::string& format, std::vector<Variant> params) const { print(INFO_LEVEL, format, std::move(params)); }
void Logger::warn(const std::string& format, std::vector<Variant> params) const { print(WARN_LEVEL, format, std::move(params)); }
void Logger::error(const std::string& format, std::vector<Variant> params) const { print(ERROR_LEVEL, format, std::move(params)); }

void Logger::print(int lvl, const std::string& format, std::vector<Variant> params) const {
    if (!isEnabled(lvl)) return;
    const std::string msg = params.empty() ? format : Format::format(format, params);
    std::lock_guard<std::mutex> lk(getMutex());
    if (sinkRef()) sinkRef()(lvl, name, msg);
    else fprintf(stderr, "nfc-laboratory %s: %s\n", name.c_str(), msg.c_str());
}

int Logger::getLevel() const { return level; }
void Logger::setLevel(int value) { level = value; }
void Logger::setLevel(const std::string& l) { level = levelOf(l); }
bool Logger::isEnabled(int l) const { return l != NONE_LEVEL && l <= level; }
bool Logger::isTraceEnabled() const { return isEnabled(TRACE_LEVEL); }
bool Logger::isDebugEnabled() const { return isEnabled(DEBUG_LEVEL); }
bool Logger::isInfoEnabled() const { return isEnabled(INFO_LEVEL); }

void Logger::init(std::ostream&, int level, bool) { setRootLevel(level); }
void Logger::shutdown() {}
void Logger::flush() {}
int Logger::getRootLevel() { return rootLevelRef(); }
void Logger::setRootLevel(int level) {
    std::lock_guard<std::mutex> lk(getMutex());
    rootLevelRef() = level;
    for (auto& kv : loggers()) kv.second->level = level;
}
void Logger::setRootLevel(const std::string& level) { setRootLevel(levelOf(level)); }
void Logger::setLoggerLevel(const std::string& target, int level) {
    std::lock_guard<std::mutex> lk(getMutex());
    getLevels()[target] = level;
    const auto it = loggers().find(target);
    if (it != loggers().end()) it->second->level = level;
}
void Logger::setLoggerLevel(const std::string& target, const std::string& level) { setLoggerLevel(target, levelOf(level)); }

Logger* Logger::getLogger(const std::string& n, int level) {
    std::lock_guard<std::mutex> lk(getMutex());
    auto& m = loggers();
    auto it = m.find(n);
    if (it == m.end()) {
        const auto lv = getLevels().find(n);
        it = m.emplace(n, std::shared_ptr<Logger>(new Logger(n, lv != getLevels().end() ? lv->second : std::min(level, rootLevelRef())))).first;
    }
    return it->second.get();
}

std::map<std::string, std::shared_ptr<Logger>>& Logger::loggers() {
    static std::map<std::string, std::shared_ptr<Logger>> m;
    return m;
}
std::mutex& Logger::getMutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, int>& Logger::getLevels() {
    static std::map<std::string, int> m;
    return m;
}

} // namespace rt

namespace nfclab {
void setLogSink(std::function<void(int level, const std::string& logger, const std::string& message)> sink) {
    static std::mutex m;   // the logger's own mutex is private; the sink is set once, before any decoder runs
    std::lock_guard<std::mutex> lk(m);
    rt::sinkRef() = std::move(sink);
}
} // namespace nfclab
