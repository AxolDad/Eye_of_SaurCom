// OnAir's stand-in for nfc-laboratory's hw/RecordDevice.cpp. The decoder uses a RecordDevice only for its signal-debug recording (a WAV
// of its internal signals, NfcSignalDebug in NfcTech.h), which OnAir never turns on; the real one brings in the whole hw-dev device layer.
// This one keeps the parameters it is given, so the debug struct's reads of them work, and records nothing.
#include <hw/RecordDevice.h>
#include <map>
#include <memory>

namespace hw {

struct RecordDevice::Impl {
    std::map<int, rt::Variant> params;
};

RecordDevice::RecordDevice(const std::string& name) : impl(std::make_shared<Impl>()) {
    impl->params[PARAM_DEVICE_NAME] = name;
    impl->params[PARAM_CHANNEL_COUNT] = (unsigned int)1;
    impl->params[PARAM_SAMPLE_RATE] = (unsigned int)0;
}

bool RecordDevice::open(Mode) { return false; }
void RecordDevice::close() {}

rt::Variant RecordDevice::get(int id, int) const {
    const auto it = impl->params.find(id);
    return it != impl->params.end() ? it->second : rt::Variant((unsigned int)0);
}

bool RecordDevice::set(int id, const rt::Variant& value, int) {
    impl->params[id] = value;
    return true;
}

bool RecordDevice::isOpen() const { return false; }
bool RecordDevice::isEof() const { return true; }
bool RecordDevice::isReady() const { return false; }
long RecordDevice::read(SignalBuffer&) { return -1; }
long RecordDevice::write(const SignalBuffer&) { return -1; }

} // namespace hw
