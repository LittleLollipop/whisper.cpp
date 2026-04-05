#include "whisper_bridge.h"
#include "whisper.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <unistd.h>
#endif

// whisper_full_default_params() caps n_threads at min(4, hardware_concurrency) — wasteful on 8–16 core Macs.
static int haven_recommended_whisper_threads(void) {
#if defined(__APPLE__)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) {
        n = 4;
    }
    if (n > 16) {
        n = 16;
    }
    return (int)n;
#else
    return 4;
#endif
}

// Implemented in Swift (AISubtitleEngine); linked into the app target.
void haven_on_whisper_progress(int progress);

static void whisper_progress_shim(struct whisper_context * ctx, struct whisper_state * state, int progress, void * user_data) {
    (void)ctx;
    (void)state;
    (void)user_data;
    haven_on_whisper_progress(progress);
}

// 简易 WAV(PCM16) 读取：支持 RIFF/WAVE，找到 fmt/data chunk。
// 要求：PCM 16-bit little endian；建议输入为 16kHz 单声道（Swift 侧导出保证）。
// 若是多声道会做简单平均；若采样率不是 16k，将返回错误（避免无质量的随意重采样）。

static uint32_t read_u32_le(FILE *fp) {
    uint8_t b[4];
    if (fread(b, 1, 4, fp) != 4) return 0;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint16_t read_u16_le(FILE *fp) {
    uint8_t b[2];
    if (fread(b, 1, 2, fp) != 2) return 0;
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

static int read_wav_pcm16_f32_mono_16k(const char *path, float **out_pcm, int *out_n_samples) {
    *out_pcm = NULL;
    *out_n_samples = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp) return 10;

    char riff[4];
    if (fread(riff, 1, 4, fp) != 4 || memcmp(riff, "RIFF", 4) != 0) { fclose(fp); return 11; }
    (void)read_u32_le(fp); // file size
    char wave[4];
    if (fread(wave, 1, 4, fp) != 4 || memcmp(wave, "WAVE", 4) != 0) { fclose(fp); return 12; }

    uint16_t audio_format = 0;
    uint16_t num_channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    uint32_t data_size = 0;
    long data_pos = 0;

    while (!feof(fp)) {
        char cid[4];
        if (fread(cid, 1, 4, fp) != 4) break;
        uint32_t csize = read_u32_le(fp);
        if (memcmp(cid, "fmt ", 4) == 0) {
            audio_format = read_u16_le(fp);
            num_channels = read_u16_le(fp);
            sample_rate = read_u32_le(fp);
            (void)read_u32_le(fp); // byte_rate
            (void)read_u16_le(fp); // block_align
            bits_per_sample = read_u16_le(fp);
            uint32_t consumed = 2 + 2 + 4 + 4 + 2 + 2;
            if (csize > consumed) fseek(fp, (long)(csize - consumed), SEEK_CUR);
        } else if (memcmp(cid, "data", 4) == 0) {
            data_size = csize;
            data_pos = ftell(fp);
            fseek(fp, (long)csize, SEEK_CUR);
        } else {
            fseek(fp, (long)csize, SEEK_CUR);
        }
        if (csize & 1) fseek(fp, 1, SEEK_CUR);
    }

    if (audio_format != 1 || bits_per_sample != 16 || data_size == 0 || data_pos == 0) { fclose(fp); return 13; }
    if (sample_rate != 16000) { fclose(fp); return 14; }
    if (num_channels < 1) { fclose(fp); return 15; }

    const uint32_t bytes_per_frame = (uint32_t)(num_channels * 2);
    const uint32_t n_frames = data_size / bytes_per_frame;
    if (n_frames == 0) { fclose(fp); return 16; }

    float *pcm = (float *)malloc(sizeof(float) * n_frames);
    if (!pcm) { fclose(fp); return 17; }

    fseek(fp, data_pos, SEEK_SET);
    for (uint32_t i = 0; i < n_frames; i++) {
        int32_t acc = 0;
        for (uint16_t ch = 0; ch < num_channels; ch++) {
            int16_t s16 = (int16_t)read_u16_le(fp);
            acc += (int32_t)s16;
        }
        float v = (float)acc / (float)num_channels;
        pcm[i] = v / 32768.0f;
    }

    fclose(fp);
    *out_pcm = pcm;
    *out_n_samples = (int)n_frames;
    return 0;
}

static void ms_to_srt_time(int64_t ms, int *h, int *m, int *s, int *ms_out) {
    *h = (int)(ms / (1000LL * 60LL * 60LL));
    *m = (int)((ms / (1000LL * 60LL)) % 60LL);
    *s = (int)((ms / 1000LL) % 60LL);
    *ms_out = (int)(ms % 1000LL);
}

static int write_srt(const char *path, struct whisper_context *ctx) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return 30;

    const int n_segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_segments; ++i) {
        const int64_t t0 = whisper_full_get_segment_t0(ctx, i); // 10ms units
        const int64_t t1 = whisper_full_get_segment_t1(ctx, i);
        const char *text = whisper_full_get_segment_text(ctx, i);

        const int64_t ms0 = t0 * 10;
        const int64_t ms1 = t1 * 10;
        int h0, m0, s0, u0;
        int h1, m1, s1, u1;
        ms_to_srt_time(ms0, &h0, &m0, &s0, &u0);
        ms_to_srt_time(ms1, &h1, &m1, &s1, &u1);

        fprintf(fp, "%d\n", i + 1);
        fprintf(fp, "%02d:%02d:%02d,%03d --> %02d:%02d:%02d,%03d\n", h0, m0, s0, u0, h1, m1, s1, u1);
        while (text && *text == ' ') text++;
        fprintf(fp, "%s\n\n", text ? text : "");
    }

    fclose(fp);
    return 0;
}

int haven_whisper_generate_srt(const char *model_path,
                               const char *audio_path,
                               const char *output_srt_path,
                               const char *language,
                               int use_gpu) {
    if (!model_path || !audio_path || !output_srt_path) return 2;

    struct whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = use_gpu != 0;
#if defined(GGML_USE_METAL) || defined(GGML_USE_CUDA) || defined(GGML_USE_VULKAN)
    if (!cparams.use_gpu) {
        cparams.flash_attn = false;
    }
#endif

    struct whisper_context *ctx = whisper_init_from_file_with_params(model_path, cparams);
    if (!ctx) return 3;

    float *pcm = NULL;
    int n_samples = 0;
    const int wav_ret = read_wav_pcm16_f32_mono_16k(audio_path, &pcm, &n_samples);
    if (wav_ret != 0) {
        whisper_free(ctx);
        return 100 + wav_ret;
    }

    struct whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads = haven_recommended_whisper_threads();
    params.print_progress   = false;
    params.print_realtime   = false;
    params.print_timestamps = false;
    params.print_special    = false;
    params.translate        = false;
    params.no_context       = true;
    params.single_segment   = false;
    params.progress_callback = whisper_progress_shim;
    params.progress_callback_user_data = NULL;
    if (language && language[0] != '\0') {
        params.language = language;
    }

    const int rc = whisper_full(ctx, params, pcm, n_samples);
    free(pcm);
    if (rc != 0) {
        whisper_free(ctx);
        return 4;
    }

    const int wrc = write_srt(output_srt_path, ctx);
    whisper_free(ctx);
    return wrc == 0 ? 0 : 5;
}

