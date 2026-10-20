#ifdef NDEBUG
#undef NDEBUG
#endif
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <cstring>
#include "../lib/ESP32-A2DP/src/SourceMediaState.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_IDF_VERSION 0
#define ESP_IDF_VERSION_VAL(a,b,c) 1
using esp_err_t=int;
constexpr int ESP_OK=0;
enum { ESP_A2D_MEDIA_CTRL_NONE,ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY,ESP_A2D_MEDIA_CTRL_START,ESP_A2D_MEDIA_CTRL_STOP,ESP_A2D_MEDIA_CTRL_SUSPEND };
enum { ESP_A2D_MEDIA_CTRL_ACK_SUCCESS,ESP_A2D_MEDIA_CTRL_ACK_FAILURE,ESP_A2D_MEDIA_CTRL_ACK_BUSY };
enum { ESP_A2D_CONNECTION_STATE_EVT,ESP_A2D_AUDIO_STATE_EVT,ESP_A2D_AUDIO_CFG_EVT,ESP_A2D_MEDIA_CTRL_ACK_EVT };
enum { ESP_A2D_CONNECTION_STATE_DISCONNECTED=0,ESP_A2D_CONNECTION_STATE_CONNECTED=2,ESP_A2D_AUDIO_STATE_STARTED=2 };
enum { APP_AV_STATE_UNCONNECTED,APP_AV_STATE_CONNECTED };
constexpr int BT_APP_HEART_BEAT_EVT=0xff00,BT_APP_MEDIA_TICK_EVT=0xff01;
struct esp_a2d_cb_param_t {
    struct {int cmd,status;} media_ctrl_stat{};
    struct {int state;} conn_stat{},audio_stat{};
};
static uint32_t clock_ms=0;
static int api_result=ESP_OK,queue_calls=0;
static bool queue_success=true;
static std::vector<int> commands;
static int esp_a2d_media_ctrl(int command){commands.push_back(command);return api_result;}
static void ccall_bt_app_av_sm_hdlr(uint16_t,void*){}
struct Frame {int16_t left=0,right=0;};
struct FakeVolume {int calls=0,lastLength=0;void update_audio_data(uint8_t*,int length){++calls;lastLength=length;}};
class BluetoothA2DPSource {
public:
    int32_t (*get_data_cb)(uint8_t*,int32_t)=nullptr;
    int32_t (*get_data_in_frames_cb)(Frame*,int32_t)=nullptr;
    FakeVolume volume;
    FakeVolume* volume_control(){return &volume;}
    int32_t get_audio_data(uint8_t*,int32_t);
    int32_t get_audio_data_volume(uint8_t*,int32_t);
    SourceMediaState source_media;
    std::atomic<bool> media_active{true},media_tick_queued{false},media_ready{false};
    std::atomic<bool> media_recovery_requested{false};
    void *app_task_queue=this;
    int s_a2d_state=APP_AV_STATE_CONNECTED,s_pkt_cnt=0;
    uint32_t get_millis(){return clock_ms;}
    bool bt_app_work_dispatch(void(*)(uint16_t,void*),uint16_t event,void*,int,void*) {
        assert(event==BT_APP_MEDIA_TICK_EVT);++queue_calls;return queue_success;
    }
    void set_media_active(bool);
    void service_media();
    void recover_media();
    void bt_app_av_media_proc(uint16_t,void*);
    void bt_app_av_state_connected_hdlr(uint16_t,void*);
    void tick(){media_tick_queued=false;bt_app_av_media_proc(BT_APP_MEDIA_TICK_EVT,nullptr);}
    void ack(int command,int status=ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
        esp_a2d_cb_param_t data{};data.media_ctrl_stat={command,status};
        bt_app_av_state_connected_hdlr(ESP_A2D_MEDIA_CTRL_ACK_EVT,&data);
    }
    void audio(int state) {
        esp_a2d_cb_param_t data{};data.audio_stat.state=state;
        bt_app_av_state_connected_hdlr(ESP_A2D_AUDIO_STATE_EVT,&data);
    }
};
