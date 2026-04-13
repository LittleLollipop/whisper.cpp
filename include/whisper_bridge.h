#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Simplified wrapper: model path, WAV path, output SRT path, language (NULL or "" = auto).
/// use_gpu: non-zero requests Metal/GPU when built with GGML_USE_METAL; zero forces CPU-only.
/// out_detected_lang: optional buffer filled with Whisper ISO-639-1 code (e.g. "en", "zh") after success; empty if unknown.
/// Returns 0 on success, non-zero on failure.
int haven_whisper_generate_srt(const char *model_path,
                               const char *audio_path,
                               const char *output_srt_path,
                               const char *language,
                               int use_gpu,
                               char *out_detected_lang,
                               size_t out_detected_lang_cap);

#ifdef __cplusplus
}
#endif

