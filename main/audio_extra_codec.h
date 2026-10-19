#pragma once
#include "audio_file.h"
typedef struct audio_extra_codec audio_extra_codec_t;
/* FLAC receives a synthetic final STREAMINFO block, bypassing large artwork
 * metadata already bounded by audio_probe_stream. AAC uses ADTS AAC-LC. */
audio_extra_codec_t *audio_extra_codec_open(audio_format_t format,const uint8_t flac_header[42]);
void audio_extra_codec_close(audio_extra_codec_t *codec);
/* 1 PCM ready, 2 consumed/waiting, 0 drained EOS, -1 malformed/unsupported.
 * Unconsumed input must remain at the same address until consumed. */
int audio_extra_codec_decode(audio_extra_codec_t *codec,const uint8_t *input,size_t bytes,bool eos,size_t *consumed);
size_t audio_extra_codec_read(audio_extra_codec_t *codec,int16_t *stereo,size_t frames,uint32_t *rng);
bool audio_extra_codec_info(audio_extra_codec_t *codec,uint32_t *rate,uint16_t *bits,uint16_t *channels);
/* Coded synthetic tones decoded in RAM; never writes audio to a device. */
bool audio_extra_codec_self_test(void);
