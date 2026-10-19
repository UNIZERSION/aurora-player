#undef NDEBUG /* zig -O1/-O2 predefines NDEBUG; keep assert() checks active */
/* Reuse the actual threaded engine/output capture harness, not a second DSP. */
#define main unused_eq_engine_main
#include "audio_eq_engine_test.c"
#undef main
int main(void){
 setvbuf(stdout,NULL,_IONBF,0);InitializeCriticalSection(&capture_lock);
 audio_reverb_config_t config=audio_reverb_default_config();assert(audio_engine_set_reverb(&config)==ESP_ERR_INVALID_STATE);
 audio_output_t out={.configure=configure,.write=output,.volume=volume};assert(audio_engine_init(&out)==ESP_OK);audio_engine_set_visuals_enabled(false);
 assert(audio_engine_set_reverb(NULL)==ESP_ERR_INVALID_ARG);config.amount=61;assert(audio_engine_set_reverb(&config)==ESP_ERR_INVALID_ARG);
 config=audio_reverb_default_config();assert(audio_engine_set_reverb(&config)==ESP_OK);
 char dir[MAX_PATH],path[MAX_PATH];assert(GetTempPathA(sizeof dir,dir));assert(GetTempFileNameA(dir,"t5r",0,path));audio_eq_config_t flat={.enabled=1};
 result_t normal=measure(path,63,4000,4000,0,flat),original=measure(path,63,4000,4000,2,flat);assert(original.hash!=normal.hash);
 config.amount=0;assert(audio_engine_set_reverb(&config)==ESP_OK);equal_pcm(normal,measure(path,63,4000,4000,2,flat));
 first_has_policy(measure(path,0,12000,0,2,flat));
 config.amount=60;config.decay_ms=4000;config.tone_hz=5000;assert(audio_engine_set_reverb(&config)==ESP_OK);
 result_t changed=measure(path,63,4000,4000,2,flat);assert(changed.hash!=original.hash&&changed.hash!=normal.hash&&changed.reduction==0);
 equal_pcm(changed,measure(path,63,4000,4000,2,flat));
 config=audio_reverb_default_config();assert(audio_engine_set_reverb(&config)==ESP_OK);equal_pcm(original,measure(path,63,4000,4000,2,flat));
 assert(DeleteFileA(path));puts("PASS actual threaded audio engine: validated setter, zero Amount bit-exact with Normal including first track sample, changed settings reach shared output PCM, settings survive track resets, original defaults restore exact PCM. Host output driver is simulated.");return 0;
}
