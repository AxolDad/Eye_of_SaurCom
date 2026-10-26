// Log to file: an action plugin. Appends every automation event (or only those whose type starts with the "prefix" setting) to events.log in its data folder.
#include "saurcom_plugin.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace {
struct Logger { const SaurComHostApi* host; std::string path; uint64_t written = 0; };

void* create(const SaurComHostApi* host, const char*) {
    char dir[1024];
    if (host->data_dir(host->ctx, dir, sizeof dir) != 0) { host->log(host->ctx, SAURCOM_LOG_ERROR, "the filesystem permission was not granted"); return nullptr; }
    auto* l = new Logger{host, std::string(dir) + "/events.log"};
    host->log(host->ctx, SAURCOM_LOG_INFO, ("writing to " + l->path).c_str());
    return l;
}
void destroy(void* p) { delete (Logger*)p; }

void onEvent(void* p, const SaurComEvent* e) {
    auto* l = (Logger*)p;
    char prefix[64] = "";
    l->host->get_setting_text(l->host->ctx, "prefix", prefix, sizeof prefix);
    if (prefix[0] && strncmp(e->type, prefix, strlen(prefix)) != 0) return;
    FILE* f = fopen(l->path.c_str(), "a");
    if (!f) return;
    fprintf(f, "%.3f %s %s %.0f", e->time, e->type, e->mode ? e->mode : "", e->freq_hz);
    for (uint32_t i = 0; i < e->n_fields; i++) {
        if (e->fields[i].is_text) fprintf(f, " %s=\"%s\"", e->fields[i].key, e->fields[i].s ? e->fields[i].s : "");
        else fprintf(f, " %s=%g", e->fields[i].key, e->fields[i].d);
    }
    fputc('\n', f);
    fclose(f);
    l->written++;
}

const SaurComPluginVTable kVt = {sizeof(SaurComPluginVTable), SAURCOM_PLUGIN_ABI_VERSION, SAURCOM_CAP_EVENTS, "log-to-file", create, destroy, nullptr, nullptr, onEvent, nullptr, nullptr};
}  // namespace

extern "C" SAURCOM_EXPORT const SaurComPluginVTable* saurcom_plugin_entry(uint32_t host_abi) { return host_abi >= SAURCOM_PLUGIN_ABI_VERSION ? &kVt : nullptr; }
