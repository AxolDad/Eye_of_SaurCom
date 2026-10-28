// Why a USB radio that is plugged in does not show up: permissions, a kernel driver that holds it (Linux).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

// One plain sentence per radio that is on the USB bus but cannot be opened, with what to do about it. Empty when nothing is wrong
// or the platform has no such check. Cheap enough to call when the radio list is refreshed.
// Linux only; with DECT2_SYSFS_ROOT set, the /sys and /dev paths are read under that folder instead (tests, on every platform).
std::vector<std::string> usbRadioHints();
// An SDRplay RSP is plugged in but the SDRplay API (its driver, which Eye of SaurCom cannot ship) is not installed: the one hint, or none (any OS)
std::vector<std::string> sdrplayApiHints();

// True for a USB id of a radio Eye of SaurCom drives (the table behind the hints above). Looks at ids only.
bool usbIsKnownRadio(uint16_t vid, uint16_t pid);

} // namespace dect2
