// macOS capture: a HAL AudioUnit on the chosen (or default) input device. Nothing is opened until audioInMicStart().
#include "audioin_impl.h"
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <AVFoundation/AVFoundation.h>
#include <string>
#include <vector>

namespace dect2 {

namespace {
AudioDeviceID parseId(const std::string& s) {
    if (s.empty()) {
        AudioDeviceID d = 0;
        UInt32 n = sizeof d;
        AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &n, &d);
        return d;
    }
    return (AudioDeviceID)strtoul(s.c_str(), nullptr, 10);
}

// the channels the input side of a device has, and the rate it runs at
UInt32 inputChannels(AudioDeviceID id) {
    AudioObjectPropertyAddress cfg = {kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeInput, kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(id, &cfg, 0, nullptr, &sz) != noErr || sz == 0) return 0;
    std::vector<uint8_t> buf(sz);
    auto* list = (AudioBufferList*)buf.data();
    if (AudioObjectGetPropertyData(id, &cfg, 0, nullptr, &sz, list) != noErr) return 0;
    UInt32 ch = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; i++) ch += list->mBuffers[i].mNumberChannels;
    return ch;
}
double nominalRate(AudioDeviceID id) {
    Float64 r = 0;
    UInt32 n = sizeof r;
    AudioObjectPropertyAddress a = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    return AudioObjectGetPropertyData(id, &a, 0, nullptr, &n, &r) == noErr ? r : 0;
}

// The unit delivers 1 or 2 channels at the device's own rate (the input unit does no sample-rate conversion, and a stereo format on a mono
// device fails the render and gives silence); a mono feed is copied to both channels and the rate converted in AudioIn::Impl::push.
struct InCtx { int chans = 2; };

static OSStatus inputCb(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32, UInt32 nFrames, AudioBufferList*) {
    auto* I = static_cast<AudioIn::Impl*>(ref);
    const int ch = static_cast<InCtx*>(I->ctx)->chans;
    if (I->scratch.size() < (size_t)nFrames * 2) I->scratch.assign((size_t)nFrames * 2, 0.f);
    AudioBufferList abl{};
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = (UInt32)ch;
    abl.mBuffers[0].mDataByteSize = (UInt32)(nFrames * (UInt32)ch * sizeof(float));
    abl.mBuffers[0].mData = I->scratch.data();
    if (AudioUnitRender((AudioUnit)I->backend, flags, ts, 1, nFrames, &abl) == noErr)
        I->push((const float*)abl.mBuffers[0].mData, nFrames, ch);
    else I->renderErrors++;
    return noErr;
}
}

bool audioInMicStart(AudioIn::Impl* I, int rate, const std::string& deviceId, std::string& err) {
    {   // microphone permission denied in System Settings gives silence, not an error: say so. A chosen input that is gone is not replaced by the default.
        const AVAuthorizationStatus st = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
        if (st == AVAuthorizationStatusDenied || st == AVAuthorizationStatusRestricted) { err = "microphone access is off for Eye of ZorCom (System Settings > Privacy & Security > Microphone)"; return false; }
        if (!deviceId.empty()) {
            bool found = false;
            for (const auto& dv : audioInListMic()) if (dv.id == deviceId) found = true;
            if (!found) { err = "the chosen microphone is not connected any more"; return false; }
        }
    }
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent c = AudioComponentFindNext(nullptr, &d);
    AudioUnit unit = nullptr;
    if (!c || AudioComponentInstanceNew(c, &unit) != noErr) { err = "CoreAudio would not open an input unit"; return false; }
    UInt32 one = 1, zero = 0;
    AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &one, sizeof one);
    AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &zero, sizeof zero);
    AudioDeviceID dev = parseId(deviceId);
    if (dev) AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof dev);
    UInt32 devCh = inputChannels(dev);
    const int chans = devCh >= 2 ? 2 : 1;
    double devRate = nominalRate(dev);
    if (devRate < 1000) devRate = rate;
    auto* ctx = new InCtx;
    ctx->chans = chans;
    I->ctx = ctx;
    AudioStreamBasicDescription f = {};
    f.mSampleRate = devRate; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mFramesPerPacket = 1; f.mChannelsPerFrame = (UInt32)chans; f.mBitsPerChannel = 32;
    f.mBytesPerFrame = 4 * (UInt32)chans; f.mBytesPerPacket = f.mBytesPerFrame;
    if (AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &f, sizeof f) != noErr) {
        AudioComponentInstanceDispose(unit);
        delete ctx; I->ctx = nullptr;
        err = "this input device would not give " + std::to_string(chans) + " channel(s) at " + std::to_string((int)devRate) + " Hz";
        return false;
    }
    if (!I->setResample(devRate, rate)) I->actualRate = (int)devRate;   // no clean ratio: the device's own rate is reported
    AURenderCallbackStruct cb = {inputCb, I};
    AudioUnitSetProperty(unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &cb, sizeof cb);
    if (AudioUnitInitialize(unit) != noErr || AudioOutputUnitStart(unit) != noErr) {
        AudioComponentInstanceDispose(unit);
        delete ctx; I->ctx = nullptr;
        err = "the microphone could not be started (check Input in System Settings)";
        return false;
    }
    I->backend = unit;
    return true;
}

void audioInMicStop(AudioIn::Impl* I) {
    AudioUnit unit = (AudioUnit)I->backend;
    if (!unit) return;
    AudioOutputUnitStop(unit);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    I->backend = nullptr;
    delete static_cast<InCtx*>(I->ctx);
    I->ctx = nullptr;
}

std::vector<AudioInputDevice> audioInListMic() {
    std::vector<AudioInputDevice> v;
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 n = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &n) != noErr || n == 0) return v;
    std::vector<AudioDeviceID> ids(n / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &n, ids.data()) != noErr) return v;
    for (AudioDeviceID id : ids) {
        AudioObjectPropertyAddress cfg = {kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeInput, kAudioObjectPropertyElementMain};
        UInt32 sz = 0;
        if (AudioObjectGetPropertyDataSize(id, &cfg, 0, nullptr, &sz) != noErr || sz == 0) continue;
        std::vector<uint8_t> buf(sz);
        auto* list = (AudioBufferList*)buf.data();
        if (AudioObjectGetPropertyData(id, &cfg, 0, nullptr, &sz, list) != noErr) continue;
        UInt32 ch = 0;
        for (UInt32 i = 0; i < list->mNumberBuffers; i++) ch += list->mBuffers[i].mNumberChannels;
        if (ch == 0) continue;
        AudioObjectPropertyAddress nm = {kAudioDevicePropertyDeviceNameCFString, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        CFStringRef cf = nullptr;
        UInt32 cfs = sizeof cf;
        std::string name = "Input";
        if (AudioObjectGetPropertyData(id, &nm, 0, nullptr, &cfs, &cf) == noErr && cf) {
            char b[256];
            if (CFStringGetCString(cf, b, sizeof b, kCFStringEncodingUTF8)) name = b;
            CFRelease(cf);
        }
        v.push_back({std::to_string((unsigned)id), name});
    }
    return v;
}

} // namespace dect2
