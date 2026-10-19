#include "audio_engine.h"
#include "audio_level_policy.h"
#include "audio_extra_codec.h"
#include "audio_speed.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "vendor/minimp3.h"

#define PCM_FRAMES 1152
#define PCM_BLOCKS 10
#define PREBUFFER_BLOCKS 4
#define INPUT_BYTES (64 * 1024)
#define SD_SECTOR_BYTES 512u
#define SD_DMA_ALIGNMENT 64u
#define SD_STAGING_BYTES (16u * 1024u)
#define SEEK_POINTS 2048
#define FFT_N 512
#define PI_F 3.14159265358979323846f

typedef enum { CMD_PLAY, CMD_PAUSE, CMD_STOP, CMD_SEEK, CMD_PAUSE_WAIT } command_type_t;
typedef struct { command_type_t type; uint32_t value; char path[AUDIO_PATH_MAX]; } command_t;
typedef struct {
    uint32_t generation, rate, end_ms;
    uint16_t frames;
    bool end;
    int16_t pcm[PCM_FRAMES * 2];
} pcm_block_t;
typedef struct { uint64_t sample; uint32_t offset; } seek_point_t;
typedef struct {
    audio_output_t output;
    QueueHandle_t commands, free_blocks, ready_blocks;
    SemaphoreHandle_t mutex;
    audio_snapshot_t snapshot;
    audio_fx_t *fx;
    audio_eq_t *eq;
    audio_eq_config_t eq_config;
    audio_reverb_config_t reverb_config;
    audio_speed_t speed;
    int16_t speed_pcm[(PCM_FRAMES*100/SPEED_MIN+2)*2];
    audio_extra_codec_t *extra;
    bool extra_test_passed;
    uint8_t flac_header[42];
    uint32_t generation;
    uint32_t next_stop_token, stopped_token;
    uint32_t next_pause_token, pause_request_token, pause_generation, pause_completed_token;
    esp_err_t pause_result;
    uint32_t decode_completed_generation;
    bool paused;
    pcm_block_t *blocks;
    uint8_t *input, *wav_raw;
    void *read_staging_allocation;
    uint8_t *read_staging;
    seek_point_t *index;
    unsigned index_count, index_stride, frame_number;
    FILE *file;
    int file_descriptor;
    bool read_failed;
    audio_file_info_t info;
    uint64_t decoded_frames, seek_target;
    size_t input_used, input_filled;
    uint64_t input_base;
    uint64_t file_read_offset;
    uint64_t step_read_us;
    bool active, eof_sent, seek_scanning, seeking;
    uint32_t rng;
    mp3dec_t decoder;
    float real[FFT_N], imag[FFT_N], hann[FFT_N];
    int16_t mono[FFT_N], visual_right[FFT_N];
    uint8_t visual_channel;
    unsigned mono_write, mono_count;
    int64_t last_visual_us;
} engine_t;
static engine_t *s;
static bool visuals_enabled=true;
void audio_engine_set_visuals_enabled(bool enabled){__atomic_store_n(&visuals_enabled,enabled,__ATOMIC_RELAXED);}

static void lock(void) { xSemaphoreTake(s->mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s->mutex); }
static uint32_t time_ms(uint64_t frames, uint32_t rate) { return rate ? (uint32_t)(frames * 1000 / rate) : 0; }
static bool quiet_state(audio_state_t state)
{ return state==AUDIO_PAUSED || state==AUDIO_STOPPED || state==AUDIO_ENDED || state==AUDIO_ERROR; }
static void notify_error(const char *message)
{
    lock();
    s->snapshot.state=AUDIO_ERROR; s->snapshot.event=AUDIO_EVENT_ERROR;
    ++s->snapshot.event_sequence;
    snprintf(s->snapshot.error,sizeof(s->snapshot.error),"%s",message);
    ++s->generation;
    unlock();
}
static void close_file(void)
{
    audio_extra_codec_close(s->extra);s->extra=NULL;
    if(s->file)fclose(s->file);
    s->file=NULL;s->file_descriptor=-1;s->read_failed=false;
    s->active=false;s->input_used=s->input_filled=0;
}
static uint32_t generation(void)
{
    lock();uint32_t v=s->generation;unlock();return v;
}
static void make_index(uint64_t sample,uint64_t offset)
{
    if (offset>UINT32_MAX) return;
    if (s->index_count && sample<=s->index[s->index_count-1].sample) return;
    if (s->frame_number++ % s->index_stride) return;
    if(s->index_count==SEEK_POINTS) {
        for(unsigned i=0;i<SEEK_POINTS/2;i++)s->index[i]=s->index[i*2];
        s->index_count=SEEK_POINTS/2;
        s->index_stride*=2;
    }
    s->index[s->index_count++]=(seek_point_t){sample,(uint32_t)offset};
}
static int reset_input(uint64_t offset)
{
    if(offset>INT32_MAX || lseek(s->file_descriptor,(off_t)offset,SEEK_SET)!=(off_t)offset)return -1;
    s->file_read_offset=offset;s->read_failed=false;
    s->input_base=offset;s->input_used=s->input_filled=0;
    mp3dec_init(&s->decoder);
    return 0;
}
static size_t read_audio(void *destination,size_t bytes)
{
    int64_t started=esp_timer_get_time();
    size_t count=0;
    while(count<bytes) {
        const size_t left=bytes-count;
        const size_t sector_offset=(size_t)(s->file_read_offset&(SD_SECTOR_BYTES-1));
        size_t chunk;
        if(sector_offset) {
            chunk=SD_SECTOR_BYTES-sector_offset;
            if(chunk>left)chunk=left;
        } else if(left>=SD_SECTOR_BYTES) {
            chunk=left&~(size_t)(SD_SECTOR_BYTES-1);
            if(chunk>SD_STAGING_BYTES)chunk=SD_STAGING_BYTES;
        } else chunk=left;
        /* FatFs first copies an unaligned file prefix from its sector cache.
         * Combining that prefix with a bulk read shifts the destination away
         * from DMA alignment. Keep prefix and suffix separate, and read every
         * whole-sector run into the same aligned internal buffer. Otherwise
         * arbitrary MP3 refill offsets trigger one SD command per 512 bytes. */
        /* Use the descriptor directly: the size-optimized newlib linked by
         * ESP-IDF turns fread(_IONBF) into one VFS read per byte. */
        ssize_t received=read(s->file_descriptor,s->read_staging,chunk);
        if(received<0) {
            if(errno==EINTR)continue;
            s->read_failed=true;
            break;
        }
        if(!received)break;
        memcpy((uint8_t*)destination+count,s->read_staging,(size_t)received);
        count+=(size_t)received;s->file_read_offset+=(size_t)received;
    }
    uint64_t elapsed=(uint64_t)(esp_timer_get_time()-started);
    s->step_read_us+=elapsed;
    uint32_t sample=elapsed>UINT32_MAX?UINT32_MAX:(uint32_t)elapsed;
    lock();if(sample>s->snapshot.max_read_us)s->snapshot.max_read_us=sample;unlock();
    return count;
}
/* Large sequential reads amortize SD/FAT overhead. Compressed bytes live in
 * PSRAM; ten decoded blocks absorb ~240 ms of SD latency at 48 kHz. */
static int fill_input(void)
{
    size_t remaining=s->input_filled-s->input_used;
    if(remaining>=16384)return 1;
    memmove(s->input,s->input+s->input_used,remaining);
    s->input_base+=s->input_used;s->input_used=0;s->input_filled=remaining;
    uint64_t end=s->info.data_offset+s->info.data_size;
    uint64_t pos=s->input_base+remaining;
    if(pos<end) {
        size_t amount=INPUT_BYTES-remaining;
        if((uint64_t)amount>end-pos)amount=(size_t)(end-pos);
        size_t n=read_audio(s->input+remaining,amount);
        s->input_filled+=n;
        if(s->read_failed)return -1;
    }
    return s->input_filled?1:0;
}
static void start_file(const char *path, bool paused)
{
    close_file();
    lock();
    ++s->generation;s->paused=paused;
    s->snapshot.state=paused?AUDIO_PAUSED:AUDIO_LOADING;s->snapshot.position_ms=0;
    s->snapshot.error[0]=0;s->snapshot.event=AUDIO_EVENT_NONE;
    s->snapshot.seek_supported=false;
    memset(s->snapshot.spectrum,0,sizeof(s->snapshot.spectrum));
    memset(s->snapshot.waveform,0,sizeof(s->snapshot.waveform));
    snprintf(s->snapshot.path,sizeof(s->snapshot.path),"%s",path);
    unlock();
    s->file=fopen(path,"rb");
    if(!s->file){notify_error("Cannot open file / SD unavailable");return;}
    /* Metadata uses buffered stdio. Streaming switches permanently to the
     * descriptor below; no stdio read/seek may follow that transition. */
    int result=audio_probe_stream(s->file,&s->info);
    if(result) {
        close_file();
        notify_error(result==-3?"File exceeds 2 GiB stdio limit":result==-1?"SD read failed":"Unsupported or damaged audio file");
        return;
    }
    if(fflush(s->file)) {close_file();notify_error("SD metadata sync failed");return;}
    s->file_descriptor=fileno(s->file);
    if(reset_input(s->info.data_offset)) {close_file();notify_error("SD seek failed");return;}
    if(s->info.format==AUDIO_FORMAT_FLAC || s->info.format==AUDIO_FORMAT_AAC) {
        if(!s->extra_test_passed){close_file();notify_error("FLAC/AAC decoder self-test failed");return;}
        if(s->info.format==AUDIO_FORMAT_FLAC) {
            if(reset_input(0) || read_audio(s->flac_header,42)!=42 || reset_input(s->info.data_offset)) {
                close_file();notify_error("FLAC STREAMINFO read failed");return;
            }
            s->flac_header[4]=0x80; /* STREAMINFO is the only metadata passed to the decoder. */
        }
        s->extra=audio_extra_codec_open(s->info.format,s->flac_header);
        if(!s->extra){close_file();notify_error("Cannot initialize FLAC/AAC decoder");return;}
    }
    s->decoded_frames=0;s->seek_target=0;s->seeking=false;s->seek_scanning=false;
    s->index_count=0;s->index_stride=64;s->frame_number=0;s->eof_sent=false;s->active=true;
    lock();
    audio_snapshot_t *p=&s->snapshot;
    p->duration_ms=s->info.duration_ms;p->duration_exact=s->info.duration_exact;
    p->sample_rate=s->info.sample_rate;p->bits_per_sample=s->info.bits_per_sample;
    p->channels=s->info.channels;p->format=s->info.format;p->file_size=s->info.file_size;
    p->bitrate_kbps=s->info.bitrate_kbps;p->seek_supported=true;p->seek_approximate=false;
    snprintf(p->title,sizeof(p->title),"%s",s->info.title);
    snprintf(p->artist,sizeof(p->artist),"%s",s->info.artist);
    snprintf(p->album,sizeof(p->album),"%s",s->info.album);
    p->event=AUDIO_EVENT_LOADED;++p->event_sequence;
    unlock();
}
static void seek_to(uint32_t ms)
{
    if(!s->file || !s->info.sample_rate)return;
    uint64_t target=(uint64_t)ms*s->info.sample_rate/1000;
    if(s->info.duration_exact && target>s->info.total_frames)target=s->info.total_frames;
    /* Report the target at once: while paused no block will update it, and
     * the UI, repeated keyboard seeks and the saved resume point read it. */
    lock();++s->generation;s->snapshot.state=s->paused?AUDIO_PAUSED:AUDIO_SEEKING;
    s->snapshot.position_ms=(uint32_t)(target*1000/s->info.sample_rate);unlock();
    s->active=true;s->eof_sent=false;s->seeking=true;s->seek_target=target;
    if(s->info.format==AUDIO_FORMAT_WAV) {
        if(reset_input(s->info.data_offset+target*s->info.block_align)) {notify_error("SD seek failed");return;}
        s->decoded_frames=target;s->seek_scanning=false;
    } else if(s->extra) {
        /* The simple decoder's parser must restart at the beginning. Decode
         * and discard up to the exact sample; commands remain interruptible. */
        audio_extra_codec_close(s->extra);
        s->extra=audio_extra_codec_open(s->info.format,s->flac_header);
        if(!s->extra || reset_input(s->info.data_offset)){notify_error("FLAC/AAC seek reset failed");return;}
        s->decoded_frames=0;s->seek_scanning=false;
    } else {
        /* Sparse checkpoints bound RAM. Forward seeking scans headers only,
         * then decodes four frames for MP3 bit-reservoir/overlap warm-up.
         * Never infer a VBR position from a linear byte percentage. */
        uint64_t warm=target>4608?target-4608:0;
        seek_point_t point={0,(uint32_t)s->info.data_offset};
        for(unsigned i=0;i<s->index_count && s->index[i].sample<=warm;i++)point=s->index[i];
        if(reset_input(point.offset)) {notify_error("SD seek failed");return;}
        s->decoded_frames=point.sample;s->seek_scanning=true;
    }
}
static void handle_command(const command_t *cmd)
{
    /* A later transport command supersedes a pending barrier. Never let an
     * old PAUSED snapshot acknowledge a new pause, resume, seek or track. */
    if(cmd->type!=CMD_PAUSE_WAIT) {
        lock();
        if(s->pause_request_token && s->pause_completed_token!=s->pause_request_token) {
            s->pause_completed_token=s->pause_request_token;
            s->pause_result=ESP_ERR_INVALID_STATE;
        }
        s->pause_request_token=0;
        unlock();
    }
    if(cmd->type==CMD_PLAY)start_file(cmd->path,cmd->value!=0);
    else if(cmd->type==CMD_SEEK)seek_to(cmd->value);
    else if(cmd->type==CMD_STOP) {
        close_file();
        lock();++s->generation;s->paused=false;s->snapshot.state=AUDIO_STOPPED;
        s->snapshot.position_ms=0;s->snapshot.event=AUDIO_EVENT_NONE;
        s->snapshot.seek_supported=false;
        if(cmd->value)s->stopped_token=cmd->value;
        memset(s->snapshot.spectrum,0,sizeof(s->snapshot.spectrum));
        memset(s->snapshot.waveform,0,sizeof(s->snapshot.waveform));unlock();
    } else if(cmd->type==CMD_PAUSE_WAIT) {
        lock();
        if(s->file || quiet_state(s->snapshot.state)) {
            s->paused=true;
            if(!quiet_state(s->snapshot.state))s->snapshot.state=AUDIO_PAUSED;
            s->pause_request_token=cmd->value;s->pause_generation=s->generation;
        } else {
            s->pause_completed_token=cmd->value;s->pause_result=ESP_ERR_INVALID_STATE;
        }
        unlock();
    } else if(cmd->type==CMD_PAUSE) {
        lock();s->paused=cmd->value!=0;
        if(s->file && s->snapshot.state!=AUDIO_ENDED && s->snapshot.state!=AUDIO_ERROR)
            s->snapshot.state=s->paused?AUDIO_PAUSED:s->seeking?AUDIO_SEEKING:AUDIO_PLAYING;
        unlock();
    }
}
/* Return 1 with PCM, 0 EOF, 2 scanning/skipping, -1 error. */
static int next_mp3(pcm_block_t *block)
{
    int filled=fill_input();
    if(filled<0)return -1;
    if(!filled)return 0;
    const uint8_t *p=s->input+s->input_used;
    size_t available=s->input_filled-s->input_used;
    if(s->seek_scanning && s->decoded_frames+4608<s->seek_target) {
        uint32_t rate;uint16_t channels,samples,br;
        int bytes=available>=4?audio_mp3_header(p,&rate,&channels,&samples,&br):0;
        if(bytes>0 && (size_t)bytes<=available && rate==s->info.sample_rate && channels==s->info.channels) {
            make_index(s->decoded_frames,s->input_base+s->input_used);
            s->input_used+=bytes;s->decoded_frames+=samples;
        } else ++s->input_used;
        return 2;
    }
    s->seek_scanning=false;
    mp3dec_frame_info_t fi={0};
    int frames=mp3dec_decode_frame(&s->decoder,p,(int)available,block->pcm,&fi);
    if(fi.frame_bytes<=0) { s->input_used=s->input_filled;return 2; }
    uint16_t declared=0;
    if(fi.frame_offset>=0 && (size_t)fi.frame_offset+4<=available)
        audio_mp3_header(p+fi.frame_offset,NULL,NULL,&declared,NULL);
    uint64_t before=s->decoded_frames;
    if(declared)make_index(before,s->input_base+s->input_used+fi.frame_offset);
    s->input_used+=(size_t)fi.frame_bytes;
    s->decoded_frames+=declared?declared:(unsigned)frames;
    if(frames<=0)return 2;
    if(fi.hz!=(int)s->info.sample_rate || fi.channels!=s->info.channels || fi.layer!=3)return -2;
    if(fi.channels==1)for(int i=frames-1;i>=0;i--)block->pcm[i*2]=block->pcm[i*2+1]=block->pcm[i];
    size_t skip=s->seeking && s->seek_target>before?(size_t)(s->seek_target-before):0;
    if(skip>=(size_t)frames)return 2;
    if(skip)memmove(block->pcm,block->pcm+skip*2,((size_t)frames-skip)*4);
    block->frames=(uint16_t)((size_t)frames-skip);
    s->seeking=false;
    return 1;
}
static int next_wav(pcm_block_t *block)
{
    if(s->decoded_frames>=s->info.total_frames)return 0;
    uint64_t remaining=s->info.total_frames-s->decoded_frames;
    size_t frames=remaining<PCM_FRAMES?(size_t)remaining:PCM_FRAMES;
    size_t bytes=frames*s->info.block_align;
    if(read_audio(s->wav_raw,bytes)!=bytes)return -1;
    audio_pcm_to_stereo16(s->wav_raw,frames,s->info.bits_per_sample,s->info.channels,&s->rng,block->pcm);
    s->decoded_frames+=frames;block->frames=(uint16_t)frames;s->seeking=false;return 1;
}
static int next_extra(pcm_block_t *block)
{
    size_t frames=audio_extra_codec_read(s->extra,block->pcm,PCM_FRAMES,&s->rng);
    if(!frames) {
        /* Preserve the address/content of all unconsumed input. Some simple
         * parsers retain it across calls; MP3's compacting refill is unsafe here. */
        if(s->input_used==s->input_filled && fill_input()<0)return -1;
        size_t available=s->input_filled-s->input_used;if(available>8192)available=8192;
        const uint64_t end=s->info.data_offset+s->info.data_size;
        const bool eos=s->input_base+s->input_used+available>=end;
        size_t consumed=0;
        int result=audio_extra_codec_decode(s->extra,s->input+s->input_used,available,eos,&consumed);
        s->input_used+=consumed;
        if(result<0)return -3;
        if(!result) {
            if(!s->decoded_frames || (s->info.format==AUDIO_FORMAT_FLAC && s->info.duration_exact && s->decoded_frames!=s->info.total_frames))return -3;
            return 0;
        }
        frames=audio_extra_codec_read(s->extra,block->pcm,PCM_FRAMES,&s->rng);
        if(!frames)return 2;
    }
    uint32_t rate;uint16_t bits,channels;
    if(!audio_extra_codec_info(s->extra,&rate,&bits,&channels) || rate!=s->info.sample_rate || channels!=s->info.channels)return -3;
    uint64_t before=s->decoded_frames;s->decoded_frames+=frames;
    size_t skip=s->seeking && s->seek_target>before?(size_t)(s->seek_target-before):0;
    if(skip>=frames)return 2;
    if(skip)memmove(block->pcm,block->pcm+skip*2,(frames-skip)*4);
    block->frames=(uint16_t)(frames-skip);s->seeking=false;return 1;
}
static void decoder_task(void *unused)
{
    (void)unused;
#ifdef ESP_PLATFORM
    const int64_t selftest_started=esp_timer_get_time();
    s->extra_test_passed=audio_extra_codec_self_test();
    ESP_LOGI("audio","CODEC_SELFTEST complete=%d elapsed_us=%lld",s->extra_test_passed,(long long)(esp_timer_get_time()-selftest_started));
#else
    s->extra_test_passed=true;
#endif
    command_t cmd;
    unsigned iterations=0;
    for(;;) {
        while(xQueueReceive(s->commands,&cmd,0)==pdTRUE)handle_command(&cmd);
        lock();bool failed=s->snapshot.state==AUDIO_ERROR;unlock();
        if(failed)s->active=false;
        if(!s->active || s->eof_sent) {
            if(xQueueReceive(s->commands,&cmd,pdMS_TO_TICKS(25))==pdTRUE)handle_command(&cmd);
            continue;
        }
        pcm_block_t *block=NULL;
        uint32_t decode_generation=generation();
        if(xQueueReceive(s->free_blocks,&block,pdMS_TO_TICKS(10))!=pdTRUE)continue;
        lock();bool cancelled=s->generation!=decode_generation || s->snapshot.state==AUDIO_ERROR;unlock();
        if(cancelled) {xQueueSend(s->free_blocks,&block,portMAX_DELAY);continue;}
        block->generation=decode_generation;block->rate=s->info.sample_rate;block->frames=0;block->end=false;
        s->step_read_us=0;
        int64_t started=esp_timer_get_time();
        int result=s->info.format==AUDIO_FORMAT_WAV?next_wav(block):s->extra?next_extra(block):next_mp3(block);
        uint64_t elapsed=(uint64_t)(esp_timer_get_time()-started);
        uint64_t decode_us=elapsed>s->step_read_us?elapsed-s->step_read_us:0;
        uint32_t decode_sample=decode_us>UINT32_MAX?UINT32_MAX:(uint32_t)decode_us;
        lock();if(decode_sample>s->snapshot.max_decode_us)s->snapshot.max_decode_us=decode_sample;unlock();
        block->end_ms=time_ms(s->decoded_frames,s->info.sample_rate);
        if(result<0) {
            xQueueSend(s->free_blocks,&block,portMAX_DELAY);
            notify_error(result==-2?"MP3 format changes mid-stream":result==-3?"Damaged or unsupported FLAC/AAC stream":"Audio read/decode failed");
        } else if(result==2)xQueueSend(s->free_blocks,&block,portMAX_DELAY);
        else {
            if(!result) {
                block->end=true;s->eof_sent=true;
                lock();s->snapshot.duration_ms=block->end_ms;s->snapshot.duration_exact=true;
                s->decode_completed_generation=block->generation;unlock();
            }
            xQueueSend(s->ready_blocks,&block,portMAX_DELAY);
        }
        /* Header-only forward seeking can otherwise monopolize a core. */
        if((++iterations&127)==0)vTaskDelay(1);
    }
}
static void transform(const int16_t *samples)
{
    for(unsigned i=0;i<FFT_N;i++) {
        s->real[i]=(float)samples[(s->mono_write+i)%FFT_N]/32768.0f*s->hann[i];s->imag[i]=0;
    }
    for(unsigned i=1,j=0;i<FFT_N;i++) {
        unsigned bit=FFT_N>>1;
        for(;j&bit;bit>>=1)j^=bit;
        j^=bit;
        if(i<j) {float t=s->real[i];s->real[i]=s->real[j];s->real[j]=t;}
    }
    for(unsigned len=2;len<=FFT_N;len<<=1) {
        float wr_step=cosf(-2*PI_F/len),wi_step=sinf(-2*PI_F/len);
        for(unsigned i=0;i<FFT_N;i+=len) {
            float wr=1,wi=0;
            for(unsigned j=0;j<len/2;j++) {
                unsigned a=i+j,b=a+len/2;
                float tr=wr*s->real[b]-wi*s->imag[b],ti=wr*s->imag[b]+wi*s->real[b];
                s->real[b]=s->real[a]-tr;s->imag[b]=s->imag[a]-ti;
                s->real[a]+=tr;s->imag[a]+=ti;
                float old=wr;wr=wr*wr_step-wi*wi_step;wi=wi*wr_step+old*wi_step;
            }
        }
    }
}
static void update_visuals(const int16_t *pcm,size_t frames)
{
    if(!__atomic_load_n(&visuals_enabled,__ATOMIC_RELAXED))return;
    for(unsigned i=0;i<frames;i++) {
        s->mono[s->mono_write]=pcm[i*2];
        s->visual_right[s->mono_write]=pcm[i*2+1];
        s->mono_write=(s->mono_write+1)%FFT_N;
        if(s->mono_count<FFT_N)++s->mono_count;
    }
    int64_t now=esp_timer_get_time();
    if(s->mono_count<FFT_N || now-s->last_visual_us<16000)return;
    s->last_visual_us=now;
    /* Averaging L+R cancels real opposite-phase stereo to silence. Use the
     * dominant channel of this exact FFT window, with 25% energy hysteresis
     * to avoid switching on nearly equal channels. One FFT, 1 KiB extra RAM;
     * this is a visual summary, not a combined stereo spectral measurement.
     * It never changes PCM delivered to the audio outputs. */
    uint64_t left_energy=0,right_energy=0;
    for(unsigned i=0;i<FFT_N;i++) {
        const int32_t left=s->mono[i],right=s->visual_right[i];
        left_energy+=(uint32_t)(left*left);right_energy+=(uint32_t)(right*right);
    }
    if(right_energy>left_energy+left_energy/4)s->visual_channel=1;
    else if(left_energy>right_energy+right_energy/4)s->visual_channel=0;
    const int16_t *samples=s->visual_channel?s->visual_right:s->mono;
    transform(samples);
    float bands[AUDIO_SPECTRUM_BANDS];
    unsigned prev=1;
    for(unsigned b=0;b<AUDIO_SPECTRUM_BANDS;b++) {
        unsigned end=(unsigned)roundf(powf((float)(FFT_N/2),(float)(b+1)/AUDIO_SPECTRUM_BANDS));
        if(end<=prev)end=prev+1;
        if(end>FFT_N/2)end=FFT_N/2;
        float peak=0;
        for(unsigned k=prev;k<end;k++) {
            float power=s->real[k]*s->real[k]+s->imag[k]*s->imag[k];
            if(power>peak)peak=power;
        }
        /* Restore only the fixed input reserve in the visual analysis. The
         * FFT still uses unclipped audio; output PCM and volume are untouched. */
        float db=10*log10f(peak*(16.0f/(FFT_N*FFT_N))+1e-12f)-AUDIO_MASTER_GAIN_DB;
        float value=(db+72)/72;
        bands[b]=value<0?0:value>1?1:value;prev=end;
    }
    lock();
    for(unsigned b=0;b<AUDIO_SPECTRUM_BANDS;b++) {
        float old=s->snapshot.spectrum[b]*0.55f;
        s->snapshot.spectrum[b]=bands[b]>old?bands[b]:old;
    }
    const float visual_gain=powf(10.0f,-AUDIO_MASTER_GAIN_DB/20.0f);
    for(unsigned i=0;i<AUDIO_WAVEFORM_SAMPLES;i++){
        float sample=samples[(s->mono_write+i*(FFT_N/AUDIO_WAVEFORM_SAMPLES))%FFT_N]*visual_gain;
        s->snapshot.waveform[i]=(int16_t)lrintf(fminf(32767,fmaxf(-32768,sample)));
    }
    unlock();
}
/* Output EQ for an effect: bands only in Custom EQ mode (effect 3), and the
 * same base gain in every mode, even with bands OFF. */
static audio_eq_config_t output_eq_config(uint8_t effect, audio_eq_config_t eq)
{
    if(effect!=3 || !eq.enabled)memset(eq.gain_db,0,sizeof(eq.gain_db));
    eq.preamp_db=AUDIO_MASTER_GAIN_DB;
    eq.enabled=1;
    return eq;
}
static void output_task(void *unused)
{
    (void)unused;
    uint32_t configured_generation=UINT32_MAX;
    int applied_volume=-1,applied_mute=-1;
    pcm_block_t *block=NULL;
    bool starving=false;
    for(;;) {
        lock();uint32_t gen=s->generation;bool paused=s->paused;
        uint8_t volume=s->snapshot.volume;
        uint8_t effect=s->snapshot.effect;
        audio_eq_config_t eq_config=output_eq_config(effect,s->eq_config);
        audio_reverb_config_t reverb_config=s->reverb_config;
        uint8_t speed=s->snapshot.speed_percent;
        bool user_mute=s->snapshot.muted;
        bool finished_decoding=s->decode_completed_generation==gen;
        bool active=s->snapshot.state==AUDIO_LOADING || s->snapshot.state==AUDIO_SEEKING || s->snapshot.state==AUDIO_PLAYING;
        bool mute=user_mute || paused || s->snapshot.state==AUDIO_STOPPED || s->snapshot.state==AUDIO_ENDED || s->snapshot.state==AUDIO_ERROR || s->snapshot.state==AUDIO_LOADING || s->snapshot.state==AUDIO_SEEKING;
        uint32_t pause_token=s->pause_request_token;
        bool pause_pending=pause_token && s->pause_completed_token!=pause_token;
        bool pause_valid=paused && quiet_state(s->snapshot.state) && s->pause_generation==gen;
        unlock();
        if(pause_pending) {
            /* This is the sole output task: reaching here means any earlier
             * configure/volume/write has returned. Require a fresh successful
             * mute even when the cached applied_mute was already true. */
            esp_err_t pause_result=pause_valid?s->output.volume(s->output.user,volume,true):ESP_ERR_INVALID_STATE;
            if(pause_valid && pause_result==ESP_OK) {applied_volume=volume;applied_mute=1;}
            lock();
            if(s->pause_request_token==pause_token && s->pause_completed_token!=pause_token) {
                if(!s->paused || !quiet_state(s->snapshot.state) || s->generation!=gen)
                    pause_result=ESP_ERR_INVALID_STATE;
                s->pause_result=pause_result;s->pause_completed_token=pause_token;
                if(pause_valid && pause_result!=ESP_OK && pause_result!=ESP_ERR_INVALID_STATE)
                    ++s->snapshot.output_errors;
            }
            unlock();
        }
        if(block && block->generation!=gen) {xQueueSend(s->free_blocks,&block,portMAX_DELAY);block=NULL;}
        if(s->output.volume && configured_generation!=UINT32_MAX && (applied_volume!=volume || applied_mute!=(int)mute)) {
            if(s->output.volume(s->output.user,volume,mute)==ESP_OK) {applied_volume=volume;applied_mute=mute;}
        }
        if(paused) {starving=false;vTaskDelay(pdMS_TO_TICKS(10));continue;}
        /* Start/seek with a modest PCM reserve (~96 ms at 48 kHz). Short
         * files may start immediately once the producer has queued EOF. */
        if(active && configured_generation!=gen && !finished_decoding &&
           uxQueueMessagesWaiting(s->ready_blocks)+(block?1u:0u)<PREBUFFER_BLOCKS) {
            starving=false;vTaskDelay(pdMS_TO_TICKS(3));continue;
        }
        if(!block && xQueueReceive(s->ready_blocks,&block,pdMS_TO_TICKS(10))!=pdTRUE) {
            lock();
            bool gap=s->generation==gen && !s->paused && s->snapshot.state==AUDIO_PLAYING;
            if(gap && !starving)++s->snapshot.buffer_starvations;
            unlock();starving=gap;continue;
        }
        starving=false;
        lock();
        bool obsolete=block->generation!=s->generation || s->snapshot.state==AUDIO_ERROR ||
                      s->snapshot.state==AUDIO_STOPPED || s->snapshot.state==AUDIO_ENDED;
        bool paused_now=s->paused;
        bool needs_prefill=configured_generation!=block->generation && s->decode_completed_generation!=block->generation;
        /* The loop-top settings can predate the blocking receive above (up to
         * 10 ms). A new stream must be seeded with the settings current for
         * the block actually processed, not the previous track's EQ/effect. */
        effect=s->snapshot.effect;eq_config=output_eq_config(effect,s->eq_config);
        reverb_config=s->reverb_config;speed=s->snapshot.speed_percent;
        unlock();
        if(obsolete) {xQueueSend(s->free_blocks,&block,portMAX_DELAY);block=NULL;continue;}
        if(paused_now)continue;
        if(needs_prefill && uxQueueMessagesWaiting(s->ready_blocks)+1u<PREBUFFER_BLOCKS) {
            vTaskDelay(pdMS_TO_TICKS(3));continue;
        }
        if(block->end) {
            lock();
            if(block->generation==s->generation) {
                s->snapshot.position_ms=block->end_ms;s->snapshot.state=AUDIO_ENDED;
                s->snapshot.event=AUDIO_EVENT_ENDED;++s->snapshot.event_sequence;
                memset(s->snapshot.spectrum,0,sizeof(s->snapshot.spectrum));
                memset(s->snapshot.waveform,0,sizeof(s->snapshot.waveform));
            }
            unlock();
            xQueueSend(s->free_blocks,&block,portMAX_DELAY);block=NULL;continue;
        }
        esp_err_t result=ESP_OK;
        if(configured_generation!=block->generation) {
            result=s->output.configure(s->output.user,block->rate);
            if(result==ESP_OK) {
                configured_generation=block->generation;s->mono_count=s->mono_write=0;
                audio_fx_reset_config(s->fx,block->rate,effect==3?0:effect,&reverb_config);audio_eq_reset_config(s->eq,block->rate,&eq_config);audio_speed_reset(&s->speed);
                memset(s->mono,0,sizeof(s->mono));memset(s->visual_right,0,sizeof(s->visual_right));s->visual_channel=0;applied_volume=applied_mute=-1;
                if(s->output.volume)result=s->output.volume(s->output.user,volume,user_mute);
            }
        }
        if(result==ESP_OK) {
            const int64_t eq_start=esp_timer_get_time();
            audio_eq_process(s->eq,block->pcm,block->frames,&eq_config);
            const float eq_reduction=audio_eq_reduction_db(s->eq);
            const uint32_t eq_us=(uint32_t)(esp_timer_get_time()-eq_start);
            // Reserve comes before Bass+/Reverb, so those modes have the same
            // input headroom and never bypass the player's fixed base level.
            const int64_t fx_start=esp_timer_get_time();
            audio_fx_set_reverb_config(s->fx,&reverb_config);
            audio_fx_process(s->fx,block->pcm,block->frames,effect==3?0:effect);
            const uint32_t fx_us=(uint32_t)(esp_timer_get_time()-fx_start);
            lock();s->snapshot.eq_last_us=eq_us;s->snapshot.eq_reduction_db=eq_reduction;
            s->snapshot.fx_last_us=fx_us;
            if(fx_us>s->snapshot.fx_max_us)s->snapshot.fx_max_us=fx_us;
            if(eq_us>s->snapshot.eq_max_us)s->snapshot.eq_max_us=eq_us;
            unlock();
            audio_speed_configure(&s->speed,speed);
            const size_t frames=audio_speed_process(&s->speed,block->pcm,block->frames,s->speed_pcm);
            update_visuals(s->speed_pcm,frames);
            result=s->output.write(s->output.user,s->speed_pcm,frames);
        }
        if(result!=ESP_OK) {
            lock();
            ++s->snapshot.output_errors;
            /* An old I2S write may finish after a new track was requested. */
            if(block->generation==s->generation) {
                s->snapshot.state=AUDIO_ERROR;s->snapshot.event=AUDIO_EVENT_ERROR;
                ++s->snapshot.event_sequence;++s->generation;
                snprintf(s->snapshot.error,sizeof(s->snapshot.error),"Audio output / I2S failed");
            }
            unlock();
        } else {
            lock();
            if(block->generation==s->generation) {
                s->snapshot.position_ms=block->end_ms;
                if(!s->paused)s->snapshot.state=AUDIO_PLAYING;
            }
            unlock();
        }
        xQueueSend(s->free_blocks,&block,portMAX_DELAY);block=NULL;
    }
}
#ifdef ESP_PLATFORM
typedef struct { uint32_t min_us,max_us,count; } eq_timing_range_t;
typedef struct {
    uint32_t rate;
    eq_timing_range_t change,fade,steady;
} eq_timing_result_t;
typedef struct {
    engine_t *engine;
    SemaphoreHandle_t complete;
    eq_timing_result_t result[3];
    uint32_t elapsed_us;
} eq_timing_context_t;

static void eq_timing_block(engine_t *e,const audio_eq_config_t *config,eq_timing_range_t *range)
{
    /* Fresh input each time: never benchmark repeatedly filtered output. */
    for(unsigned i=0;i<PCM_FRAMES*2;++i)e->blocks[0].pcm[i]=(i&32)?24000:-24000;
    const int64_t begin=esp_timer_get_time();
    audio_eq_process(e->eq,e->blocks[0].pcm,PCM_FRAMES,config);
    const uint32_t elapsed=(uint32_t)(esp_timer_get_time()-begin);
    if(range){
        if(!range->count || elapsed<range->min_us)range->min_us=elapsed;
        if(elapsed>range->max_us)range->max_us=elapsed;
        ++range->count;
    }
}

static void eq_timing_task(void *opaque)
{
    eq_timing_context_t *context=opaque;
    engine_t *e=context->engine;
    const int64_t begin=esp_timer_get_time();
    for(unsigned ri=0;ri<3;++ri){
        eq_timing_result_t *result=&context->result[ri];
        result->rate=ri==0?44100:ri==1?48000:96000;
        audio_eq_config_t probe={.enabled=1};
        for(unsigned i=0;i<AUDIO_EQ_BANDS;++i)probe.gain_db[i]=12;
        audio_eq_reset(e->eq,result->rate);
        /* Two blocks finish the initial bypass fade even at 96 kHz. */
        eq_timing_block(e,&probe,NULL);
        eq_timing_block(e,&probe,NULL);
        for(unsigned trial=0;trial<4;++trial){
            for(unsigned i=0;i<AUDIO_EQ_BANDS;++i)
                probe.gain_db[i]=(trial&1) || (i&1)?12:-12;
            /* Both banks have eleven active sections, unlike bypass->EQ. */
            eq_timing_block(e,&probe,&result->change);
            if(e->eq->transition)eq_timing_block(e,&probe,&result->fade);
            eq_timing_block(e,&probe,&result->steady);
        }
    }
    context->elapsed_us=(uint32_t)(esp_timer_get_time()-begin);
    /* No context/engine access after signaling: the caller may resume on
     * the other core and immediately release this stack-owned context. */
    xSemaphoreGive(context->complete);
    vTaskDelete(NULL);
}

static void eq_timing_boot(engine_t *e)
{
    /* Bounded boot-only diagnostic, before decoder/output tasks exist.
     * Match the real output task's core and priority; do not suppress IRQs
     * or the scheduler. Durations include any remaining preemption/cache cost
     * and do not establish a universal audio real-time guarantee. */
    eq_timing_context_t context={.engine=e};
    context.complete=xSemaphoreCreateBinary();
    if(!context.complete){ESP_LOGW("audio","EQ_TIMING skipped: no semaphore");return;}
    if(xTaskCreatePinnedToCore(eq_timing_task,"eq_boot_probe",8192,&context,7,NULL,1)!=pdPASS){
        vSemaphoreDelete(context.complete);
        ESP_LOGW("audio","EQ_TIMING skipped: no task");return;
    }
    xSemaphoreTake(context.complete,portMAX_DELAY);
    vSemaphoreDelete(context.complete);
    for(unsigned ri=0;ri<3;++ri){
        const eq_timing_result_t *r=&context.result[ri];
        ESP_LOGI("audio","EQ_TIMING rate=%lu trials=4 change_min_us=%lu change_max_us=%lu fade_min_us=%lu fade_max_us=%lu fade_blocks=%lu steady_min_us=%lu steady_max_us=%lu block_budget_us=%lu speed125_budget_us=%lu",
                 (unsigned long)r->rate,(unsigned long)r->change.min_us,(unsigned long)r->change.max_us,
                 (unsigned long)r->fade.min_us,(unsigned long)r->fade.max_us,(unsigned long)r->fade.count,
                 (unsigned long)r->steady.min_us,(unsigned long)r->steady.max_us,
                 (unsigned long)(1000000ULL*PCM_FRAMES/r->rate),
                 (unsigned long)(1000000ULL*PCM_FRAMES*100/(r->rate*125ULL)));
    }
    ESP_LOGI("audio","EQ_TIMING complete_us=%lu core=1 priority=7; budgets also serve resampler/visuals/output",
             (unsigned long)context.elapsed_us);
}
#endif

esp_err_t audio_engine_init(const audio_output_t *output)
{
    if(s)return ESP_ERR_INVALID_STATE;
    if(!output || !output->configure || !output->write)return ESP_ERR_INVALID_ARG;
    engine_t *e=heap_caps_calloc(1,sizeof(*e),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!e)return ESP_ERR_NO_MEM;
    e->output=*output;e->reverb_config=audio_reverb_default_config();
    e->fx=heap_caps_calloc(1,sizeof(audio_fx_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    e->eq=heap_caps_calloc(1,sizeof(audio_eq_t),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    e->input=heap_caps_malloc(INPUT_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    e->blocks=heap_caps_calloc(PCM_BLOCKS,sizeof(pcm_block_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    e->index=heap_caps_calloc(SEEK_POINTS,sizeof(seek_point_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    e->wav_raw=heap_caps_malloc(PCM_FRAMES*8,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    e->read_staging_allocation=heap_caps_malloc(SD_STAGING_BYTES+SD_DMA_ALIGNMENT-1,
                                               MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT);
    if(e->read_staging_allocation)
        e->read_staging=(uint8_t*)(((uintptr_t)e->read_staging_allocation+SD_DMA_ALIGNMENT-1)&~(uintptr_t)(SD_DMA_ALIGNMENT-1));
    e->mutex=xSemaphoreCreateMutex();e->commands=xQueueCreate(8,sizeof(command_t));
    e->free_blocks=xQueueCreate(PCM_BLOCKS,sizeof(pcm_block_t*));
    e->ready_blocks=xQueueCreate(PCM_BLOCKS,sizeof(pcm_block_t*));
    if(!e->eq || !e->fx || !e->input || !e->blocks || !e->index || !e->wav_raw || !e->read_staging || !e->mutex || !e->commands || !e->free_blocks || !e->ready_blocks) {
        if(e->mutex)vSemaphoreDelete(e->mutex);
        if(e->commands)vQueueDelete(e->commands);
        if(e->free_blocks)vQueueDelete(e->free_blocks);
        if(e->ready_blocks)vQueueDelete(e->ready_blocks);
        free(e->eq);free(e->fx);free(e->input);free(e->blocks);free(e->index);free(e->wav_raw);free(e->read_staging_allocation);free(e);return ESP_ERR_NO_MEM;
    }
    e->snapshot.speed_percent=100;e->snapshot.volume=68;e->snapshot.state=AUDIO_STOPPED;e->rng=0x9a341c11;
    for(unsigned i=0;i<FFT_N;i++)e->hann[i]=0.5f-0.5f*cosf(2*PI_F*i/(FFT_N-1));
    for(unsigned i=0;i<PCM_BLOCKS;i++){pcm_block_t *block=&e->blocks[i];xQueueSend(e->free_blocks,&block,0);}
    s=e;
#ifdef ESP_PLATFORM
    eq_timing_boot(e);
#endif
    TaskHandle_t decoder=NULL;
    if(xTaskCreatePinnedToCore(decoder_task,"audio_decode",32768,NULL,6,&decoder,1)!=pdPASS ||
       xTaskCreatePinnedToCore(output_task,"audio_output",8192,NULL,7,NULL,1)!=pdPASS) {
        if(decoder)vTaskDelete(decoder);
        s=NULL;vSemaphoreDelete(e->mutex);vQueueDelete(e->commands);vQueueDelete(e->free_blocks);vQueueDelete(e->ready_blocks);
        free(e->eq);free(e->fx);free(e->input);free(e->blocks);free(e->index);free(e->wav_raw);free(e->read_staging_allocation);free(e);return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
static esp_err_t send(command_type_t type,uint32_t value,const char *path)
{
    if(!s)return ESP_ERR_INVALID_STATE;
    command_t command={.type=type,.value=value};
    if(path) {
        size_t length=strlen(path);
        if(!length || length>=sizeof(command.path))return ESP_ERR_INVALID_ARG;
        memcpy(command.path,path,length+1);
    }
    return xQueueSend(s->commands,&command,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}
esp_err_t audio_engine_play_paused(const char *path,bool paused) {return path?send(CMD_PLAY,paused?1:0,path):ESP_ERR_INVALID_ARG;}
esp_err_t audio_engine_play(const char *path) {return audio_engine_play_paused(path,false);}
esp_err_t audio_engine_pause(bool paused) {return send(CMD_PAUSE,paused,NULL);}
esp_err_t audio_engine_pause_and_wait(uint32_t timeout_ms)
{
    if(!s)return ESP_ERR_INVALID_STATE;
    if(!s->output.volume)return ESP_ERR_NOT_SUPPORTED;
    lock();uint32_t token=++s->next_pause_token;
    if(!token)token=++s->next_pause_token;
    unlock();
    esp_err_t result=send(CMD_PAUSE_WAIT,token,NULL);
    if(result!=ESP_OK)return result;
    int64_t deadline=esp_timer_get_time()+(int64_t)timeout_ms*1000;
    for(;;) {
        lock();
        bool done=s->pause_completed_token==token;
        result=s->pause_result;
        if(done && result==ESP_OK && (s->pause_request_token!=token || !s->paused ||
           !quiet_state(s->snapshot.state) || s->pause_generation!=s->generation))
            result=ESP_ERR_INVALID_STATE;
        bool superseded=s->next_pause_token!=token;
        unlock();
        if(superseded)return ESP_ERR_INVALID_STATE;
        if(done)return result;
        if(esp_timer_get_time()>=deadline)return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
}
esp_err_t audio_engine_stop(void) {return send(CMD_STOP,0,NULL);}
esp_err_t audio_engine_stop_and_wait(uint32_t timeout_ms)
{
    if(!s)return ESP_ERR_INVALID_STATE;
    lock();uint32_t token=++s->next_stop_token;
    if(!token)token=++s->next_stop_token;
    unlock();
    esp_err_t result=send(CMD_STOP,token,NULL);
    if(result!=ESP_OK)return result;
    int64_t deadline=esp_timer_get_time()+(int64_t)timeout_ms*1000;
    for(;;) {
        lock();bool done=(int32_t)(s->stopped_token-token)>=0;unlock();
        if(done)return ESP_OK;
        if(esp_timer_get_time()>=deadline)return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
}
esp_err_t audio_engine_seek_ms(uint32_t ms) {return send(CMD_SEEK,ms,NULL);}
esp_err_t audio_engine_set_volume(uint8_t percent,bool muted)
{
    if(!s)return ESP_ERR_INVALID_STATE;
    if(percent>100)return ESP_ERR_INVALID_ARG;
    lock();s->snapshot.volume=percent;s->snapshot.muted=muted;unlock();return ESP_OK;
}
esp_err_t audio_engine_set_eq(const audio_eq_config_t *config)
{
    if(!s)return ESP_ERR_INVALID_STATE;
    if(!audio_eq_valid_config(config))return ESP_ERR_INVALID_ARG;
    lock();s->eq_config=*config;unlock();return ESP_OK;
}
esp_err_t audio_engine_set_effect(uint8_t effect)
{
    if(!s)return ESP_ERR_INVALID_STATE;
    if(effect>AUDIO_EFFECT_RETRO)return ESP_ERR_INVALID_ARG;
    lock();s->snapshot.effect=effect;unlock();return ESP_OK;
}
esp_err_t audio_engine_set_reverb(const audio_reverb_config_t *config)
{
    if(!s)return ESP_ERR_INVALID_STATE;
    if(!audio_reverb_valid_config(config))return ESP_ERR_INVALID_ARG;
    lock();s->reverb_config=*config;unlock();return ESP_OK;
}
esp_err_t audio_engine_set_speed(uint8_t percent)
{
    if(!s)return ESP_ERR_INVALID_STATE;
    if(percent<SPEED_MIN||percent>SPEED_MAX)return ESP_ERR_INVALID_ARG;
    lock();s->snapshot.speed_percent=percent;unlock();return ESP_OK;
}
void audio_engine_get_snapshot(audio_snapshot_t *out)
{
    if(!out)return;
    if(!s){memset(out,0,sizeof(*out));return;}
    lock();*out=s->snapshot;unlock();
    out->buffered_blocks=(uint8_t)uxQueueMessagesWaiting(s->ready_blocks);
}
