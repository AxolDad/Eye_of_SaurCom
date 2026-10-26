// A do-nothing IQ plugin: counts the samples it gets and shows the number as a status line.
#include "saurcom_plugin.h"
#include <cstdio>

namespace {
struct State { const SaurComHostApi* host; unsigned long long samples = 0; };

void* create(const SaurComHostApi* host, const char* mode_id) { (void)mode_id; return new State{host}; }
void destroy(void* p) { delete (State*)p; }
void process(void* p, const float* iq, uint32_t n, const SaurComIqMeta* meta) {
    (void)iq; (void)meta;
    auto* s = (State*)p;
    s->samples += n;
    char text[64];
    snprintf(text, sizeof text, "%llu samples", s->samples);
    s->host->status(s->host->ctx, "samples", text, SAURCOM_LAMP_OK);
}
const SaurComPluginVTable kVt = {sizeof(SaurComPluginVTable), SAURCOM_PLUGIN_ABI_VERSION, SAURCOM_CAP_IQ_SINK, "my-plugin", create, destroy, nullptr, process, nullptr, nullptr, nullptr};
}  // namespace

extern "C" SAURCOM_EXPORT const SaurComPluginVTable* saurcom_plugin_entry(uint32_t host_abi) { return host_abi >= SAURCOM_PLUGIN_ABI_VERSION ? &kVt : nullptr; }
