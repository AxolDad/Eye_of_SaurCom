// Where nfc-laboratory's log lines go in Eye of ZorCom (see zorcom/Logger.cpp): a sink for the WARN and ERROR lines of its decoder. Without one
// they go to stderr. level: rt::Logger::Level (1 error, 2 warn, 3 info, ...).
#pragma once
#include <functional>
#include <string>

namespace nfclab {
void setLogSink(std::function<void(int level, const std::string& logger, const std::string& message)> sink);
}
