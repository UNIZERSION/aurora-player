#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { AUDIO_FORMAT_UNKNOWN, AUDIO_FORMAT_MP3, AUDIO_FORMAT_WAV,
               AUDIO_FORMAT_FLAC, AUDIO_FORMAT_AAC } audio_format_t;
typedef struct {
    audio_format_t format;
    uint32_t sample_rate, duration_ms, bitrate_kbps;
    uint16_t bits_per_sample, channels, block_align;
    uint64_t file_size, data_offset, data_size, total_frames;
    bool duration_exact;
    char title[128], artist[128], album[128];
} audio_file_info_t;

/* No ESP-IDF dependency; 0 = success, -1 = I/O, -2 = malformed/unsupported
 * or metadata search limit exceeded, -3 = too large for target stdio offsets.
 * Probes regular files using fstat (no seek-to-end), at most 192 bounded reads,
 * 64 ID3 frames, a 16 MiB header/tag offset and 256 KiB MP3 sync search. Optional
 * WAV metadata after the audio payload is skipped to avoid full-file seeks.
 * A filesystem/driver read itself must provide its own timeout. */
int audio_probe_file(const char *path, audio_file_info_t *out);
int audio_probe_stream(FILE *file, audio_file_info_t *out);

/* Validates an MPEG Layer III frame header. Returns frame byte count, or 0.
 * Free-format MP3 is intentionally unsupported: no reliable bounded seek. */
int audio_mp3_header(const uint8_t h[4], uint32_t *rate, uint16_t *channels,
                     uint16_t *samples, uint16_t *bitrate_kbps);
/* AAC-LC ADTS, one raw data block, explicit mono/stereo configuration only. */
int audio_aac_header(const uint8_t h[7],uint32_t *rate,uint16_t *channels);

/* Converts little-endian integer PCM to interleaved signed 16-bit stereo.
 * 24/32-bit reduction uses one-LSB TPDF dither. Mono is duplicated. */
void audio_pcm_to_stereo16(const uint8_t *src, size_t frames,
                           uint16_t bits, uint16_t channels,
                           uint32_t *rng, int16_t *dst);
#ifdef __cplusplus
}
#endif
