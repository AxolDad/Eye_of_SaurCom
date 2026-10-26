// thread_name.h: the name is set on the thread, kept short on Linux, and namedThread names its thread
#include "dect2/common/thread_name.h"
#include <cstdio>
#include <string>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main() {
    CHECK(dect2::clipThreadName("zorcom-short") == "zorcom-short");
    CHECK(dect2::clipThreadName("zorcom-a-very-long-thread-name", 15) == "zorcom-a-very-lo");
    CHECK(dect2::clipThreadName(nullptr).empty());
#if !defined(_WIN32)
    std::string seen;
    std::thread t = dect2::namedThread("zorcom-test", [&seen](int) { seen = dect2::currentThreadName(); }, 1);
    t.join();
    CHECK(seen == "zorcom-test");
    std::string longSeen;
    std::thread u = dect2::namedThread("zorcom-this-name-is-way-too-long-for-linux-and-macos-limits-both-xxxxxxxxxxxxxxxxxxxxxxxx", [&] { longSeen = dect2::currentThreadName(); });
    u.join();
    CHECK(!longSeen.empty() && longSeen.size() <= dect2::kThreadNameMax);   // cut, never dropped
    CHECK(longSeen.rfind("zorcom-this-na", 0) == 0);
    dect2::setThreadName("zorcom-main");
    CHECK(dect2::currentThreadName() == "zorcom-main");
    dect2::setThreadName("");   // ignored
    CHECK(dect2::currentThreadName() == "zorcom-main");
#endif
    if (!fails) std::printf("thread_name OK\n");
    return fails ? 1 : 0;
}
