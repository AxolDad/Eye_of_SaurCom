// A do-nothing IQ plugin: counts the samples it gets and shows the number as a status line.
#include "zorcom_plugin.h"
#include <cstdio>

namespace {
struct State { const ZorComHostApi* host; unsigned long long samples = 0; };

void* create(const ZorComHostApi* host, const char* mode_id) { (void)mode_id; return new State{host}; }
void destroy(void* p) { delete (State*)p; }
void process(void* p, const float* iq, uint32_t n, const ZorComIqMeta* meta) {
    (void)iq; (void)meta;
    auto* s = (State*)p;
    s->samples += n;
    char text[64];
    snprintf(text, sizeof text, "%llu samples", s->samples);
    s->host->status(s->host->ctx, "samples", text, ZORCOM_LAMP_OK);
}
const ZorComPluginVTable kVt = {sizeof(ZorComPluginVTable), ZORCOM_PLUGIN_ABI_VERSION, ZORCOM_CAP_IQ_SINK, "my-plugin", create, destroy, nullptr, process, nullptr, nullptr, nullptr};
}  // namespace

extern "C" ZORCOM_EXPORT const ZorComPluginVTable* zorcom_plugin_entry(uint32_t host_abi) { return host_abi >= ZORCOM_PLUGIN_ABI_VERSION ? &kVt : nullptr; }
