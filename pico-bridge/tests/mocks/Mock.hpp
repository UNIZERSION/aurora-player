#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <deque>
#include <map>
#include <vector>
#include <functional>
#include <stdexcept>
using String=std::string;
using esp_err_t=int; constexpr int ESP_OK=0, ESP_ERR_NO_MEM=0x101;
constexpr int GPIO_NUM_32=32, RTC_GPIO_MODE_INPUT_ONLY=0, SERIAL_8N1=0;
constexpr int ESP_RST_SW=3;
#define RTC_NOINIT_ATTR
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdMS_TO_TICKS(x) (x)
#define pdPASS 1
inline uint32_t mock_ms=0; inline uint32_t millis(){return mock_ms;}
inline void delay(unsigned n){mock_ms+=n;}
inline void vTaskDelay(unsigned n){mock_ms+=n;}
inline int xTaskCreatePinnedToCore(void(*)(void*),const char*,int,void*,int,void*,int){return 1;}
struct Reset:std::exception{};
struct ESPClass{void restart(){throw Reset{};} uint32_t getFreeHeap(){return 150000;}};inline ESPClass ESP;
inline int esp_reset_reason(){return 0;}
struct Print {
 std::string output; virtual ~Print()=default;
 void println(const char*s){output+=s;output+='\n';}
 int printf(const char*f,...){char buf[2048];va_list a;va_start(a,f);int n=vsnprintf(buf,sizeof(buf),f,a);va_end(a);output+=buf;return n;}
 void flush(){};
};
struct HardwareSerial:Print{std::deque<char> input;void begin(int,int=0,int=0,int=0){};void end(){};int available(){return int(input.size());}int read(){char c=input.front();input.pop_front();return c;}};
inline HardwareSerial Serial,Serial2;
struct Preferences {
 std::map<std::string,std::vector<uint8_t>> values;
 bool begin(const char*,bool){return true;}
 size_t putBytes(const char*k,const void*v,size_t n){const auto*b=(const uint8_t*)v;values[k]={b,b+n};return n;}
 size_t getBytesLength(const char*k){return values[k].size();}
 size_t getBytes(const char*k,void*v,size_t n){n=std::min(n,values[k].size());memcpy(v,values[k].data(),n);return n;}
 bool remove(const char*k){return values.erase(k)>0;}
 uint8_t getUChar(const char*k,uint8_t v=0){return values[k].empty()?v:values[k][0];}
 size_t putUChar(const char*k,uint8_t v){return putBytes(k,&v,1);}
 bool getBool(const char*k,bool v=false){return getUChar(k,v);}
 size_t putBool(const char*k,bool v){return putUChar(k,v);}
 String getString(const char*k,const char*v){return values[k].empty()?String(v):String((char*)values[k].data());}
 size_t putString(const char*k,const char*v){return putBytes(k,v,strlen(v)+1);}
};
using esp_bd_addr_t=uint8_t[6];
inline int esp_bt_gap_get_bond_device_num(){return 0;}
inline int esp_bt_gap_get_bond_device_list(int*,esp_bd_addr_t*){return 0;}
using esp_a2d_connection_state_t=int; using esp_a2d_audio_state_t=int;
constexpr int ESP_A2D_CONNECTION_STATE_DISCONNECTED=0,ESP_A2D_CONNECTION_STATE_CONNECTED=2;
#ifdef MOCK_NATIVE_AUDIO_ENUM
// ESP-IDF 5.5 merged STOPPED/REMOTE_SUSPEND into SUSPEND and moved STARTED to 1.
constexpr int ESP_A2D_AUDIO_STATE_SUSPEND=0,ESP_A2D_AUDIO_STATE_STOPPED=0,ESP_A2D_AUDIO_STATE_STARTED=1;
#else
constexpr int ESP_A2D_AUDIO_STATE_SUSPEND=1,ESP_A2D_AUDIO_STATE_STOPPED=1,ESP_A2D_AUDIO_STATE_STARTED=2;
#endif
constexpr int ESP_AVRC_PT_CMD_PLAY=0x44,ESP_AVRC_PT_CMD_STOP=0x45,ESP_AVRC_PT_CMD_PAUSE=0x46,ESP_AVRC_PT_CMD_FORWARD=0x4B,ESP_AVRC_PT_CMD_BACKWARD=0x4C,ESP_AVRC_PT_CMD_STATE_RELEASED=1;
struct Frame {int16_t channel1=0,channel2=0;};
struct A2DPNoVolumeControl{};
struct BluetoothA2DPSource {
 bool media_active=true; unsigned media_services=0,media_recoveries=0;
 void set_media_active(bool active){media_active=active;}
 void service_media(){++media_services;}
 void recover_media(){++media_recoveries;}
 bool is_media_active()const{return media_active;}
 void set_local_name(const char*){};void set_auto_reconnect(uint8_t*,int){};void set_auto_reconnect(bool){};
 void set_ssp_enabled(bool){};void set_discovery_enabled(bool){};
 void set_ssid_callback(bool(*)(const char*,esp_bd_addr_t,int)){};
 void set_on_connection_state_changed(void(*)(int,void*)){};void set_on_audio_state_changed(void(*)(int,void*)){};
 void set_avrc_passthru_command_callback(void(*)(uint8_t,bool)){};void set_data_callback_in_frames(int32_t(*)(Frame*,int32_t)){};
 void set_volume_control(A2DPNoVolumeControl*){};void start(){};
};
using i2s_mode_t=int;constexpr int I2S_NUM_0=0,I2S_MODE_SLAVE=1,I2S_MODE_RX=2,I2S_BITS_PER_SAMPLE_16BIT=16,I2S_CHANNEL_FMT_RIGHT_LEFT=0,I2S_COMM_FORMAT_STAND_I2S=0,ESP_INTR_FLAG_LEVEL1=1,I2S_PIN_NO_CHANGE=-1;
struct i2s_config_t{int mode,sample_rate,bits_per_sample,channel_format,communication_format,intr_alloc_flags,dma_buf_count,dma_buf_len;bool use_apll;};
struct i2s_pin_config_t{int bck_io_num,ws_io_num,data_out_num,data_in_num;};
inline int i2s_driver_install(int,const i2s_config_t*,int,void*){return 0;}
inline int i2s_set_pin(int,const i2s_pin_config_t*){return 0;}
inline std::function<int(void*,size_t,size_t*)> mock_read;
inline int i2s_read(int,void*p,size_t n,size_t*b,int){return mock_read(p,n,b);}
inline void rtc_gpio_init(int){};inline void rtc_gpio_deinit(int){};inline void rtc_gpio_set_direction(int,int){};inline void rtc_gpio_pulldown_dis(int){};inline void rtc_gpio_pullup_en(int){};inline int rtc_gpio_get_level(int){return 1;}
inline void esp_sleep_enable_ext0_wakeup(int,int){};inline void esp_deep_sleep_start(){throw Reset{};}
