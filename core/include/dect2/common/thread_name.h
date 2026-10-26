// Names the threads of the app so profilers, debuggers, Activity Monitor, htop and crash reports show what each one does.
//
//   setThreadName("zorcom-dec")        names the calling thread (call first thing inside a thread function)
//   namedThread("zorcom-rec", fn, a..) std::thread that runs fn(a...) and is named before it starts working
//
// Linux keeps 15 characters (the kernel limit; longer names would make pthread_setname_np fail, so they are cut), macOS 63, Windows uses
// SetThreadDescription (Windows 10 1607+, looked up at run time so older systems just skip it), Android is Linux. Use short stable names
// "zorcom-<what>", with the receiver number appended where several run ("zorcom-rx2"). No behaviour change: naming never fails or throws.
#pragma once
#include <cstddef>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace dect2 {

// The name as it is kept for a limit of maxLen characters (maxLen 15 = Linux)
inline std::string clipThreadName(const char* name, size_t maxLen = 15) {
    std::string s = name ? name : "";
    if (s.size() > maxLen) s.resize(maxLen);
    return s;
}

// The longest name the platform keeps
constexpr size_t kThreadNameMax =
#if defined(__APPLE__)
    63;
#elif defined(_WIN32)
    256;
#else
    15;
#endif

inline void setThreadName(const char* name) {
    if (!name || !*name) return;
#if defined(_WIN32)
    using Fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const Fn fn = [] {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        return k ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(k, "SetThreadDescription"))) : nullptr;
    }();
    if (!fn) return;
    wchar_t w[kThreadNameMax + 1];
    size_t n = 0;
    for (; name[n] && n < kThreadNameMax; n++) w[n] = (unsigned char)name[n];
    w[n] = 0;
    fn(GetCurrentThread(), w);
#elif defined(__APPLE__)
    pthread_setname_np(clipThreadName(name, kThreadNameMax).c_str());
#else
    pthread_setname_np(pthread_self(), clipThreadName(name, kThreadNameMax).c_str());
#endif
}

// The name the calling thread has now (empty when the platform cannot tell); used by the tests
inline std::string currentThreadName() {
#if defined(_WIN32)
    return {};
#else
    char b[80] = {0};
    if (pthread_getname_np(pthread_self(), b, sizeof b) != 0) return {};
    return b;
#endif
}

// std::thread that names itself first. Arguments are copied like std::thread does.
template <class F, class... A>
std::thread namedThread(const char* name, F&& fn, A&&... args) {
    return std::thread([n = std::string(name ? name : ""), f = std::decay_t<F>(std::forward<F>(fn)),
                        tup = std::make_tuple(std::decay_t<A>(std::forward<A>(args))...)]() mutable {
        setThreadName(n.c_str());
        std::apply(std::move(f), std::move(tup));
    });
}

}  // namespace dect2
