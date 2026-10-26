// Eye of SaurCom: the ANEForge encoder (an experimental Apple Neural Engine back end that loads a library at run time) is not part of this vendored copy.
// These stubs keep whisper.cpp unchanged; whisper_aneforge_init() always fails, so the normal CPU encoder is used.
#include "whisper-aneforge.h"

struct whisper_aneforge_context * whisper_aneforge_init(const char *) { return nullptr; }
void whisper_aneforge_encode(struct whisper_aneforge_context *, int64_t, int64_t, const float *, float *) {}
void whisper_aneforge_free(struct whisper_aneforge_context *) {}
