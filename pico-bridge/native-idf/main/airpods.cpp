#include "airpods.h"
#include "AirPodsProtocol.hpp"
#include "CommandLine.hpp"
#ifndef PICO_AIRPODS_HOST_TEST
#include "Preferences.h"
#include "esp_bt_main.h"
#include "esp_a2dp_api.h"
#include "esp_l2cap_bt_api.h"
#include <unistd.h>
#endif
#include <atomic>
#include <cctype>

extern std::atomic<int> connection;
namespace {
std::atomic<bool> ready{false},pending{false},requested{false},autoPause{false};
std::atomic<bool> retryRequested{false};
std::atomic<int> newFd{-1},pendingMode{0},linkState{0};
std::atomic<uint32_t> openHandle{0},closedHandle{0};
uint8_t peer[6]{};
airpods::State published;
portMUX_TYPE statusLock=portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t task=nullptr;
Preferences settings;
std::atomic<int> codecRate{-1},codecChannels{-1},codecMin{-1},codecMax{-1};

void callback(esp_bt_l2cap_cb_event_t event,esp_bt_l2cap_cb_param_t* p) {
  if(event==ESP_BT_L2CAP_INIT_EVT && p->init.status==ESP_BT_L2CAP_SUCCESS)
    esp_bt_l2cap_vfs_register();
  else if(event==ESP_BT_L2CAP_VFS_REGISTER_EVT)
    ready.store(p->vfs_register.status==ESP_BT_L2CAP_SUCCESS);
  else if(event==ESP_BT_L2CAP_OPEN_EVT) {
    pending.store(false);
    if(p->open.status==ESP_BT_L2CAP_SUCCESS && !memcmp(peer,p->open.rem_bda,6)) {
      closedHandle.store(0);openHandle.store(p->open.handle);newFd.store(p->open.fd);
    } else linkState.store(0);
  } else if(event==ESP_BT_L2CAP_CLOSE_EVT)closedHandle.store(p->close.handle);
  else if(event==ESP_BT_L2CAP_CL_INIT_EVT && p->cl_init.status!=ESP_BT_L2CAP_SUCCESS)
    pending.store(false);
}
void publish(const airpods::State& state) {
  portENTER_CRITICAL(&statusLock);published=state;portEXIT_CRITICAL(&statusLock);
  airpods_status(Serial2);
}
void worker(void*) {
  int fd=-1;uint32_t handle=0,lastAttempt=0,connectedAt=0;
  bool wasConnected=false,initialized=false;unsigned attempts=0;
  unsigned awaitingMode=0;uint32_t modeSentAt=0;
  airpods::Parser parser;
  for(;;) {
    const uint32_t now=millis();
    const bool connected=connection.load()==ESP_A2D_CONNECTION_STATE_CONNECTED;
    if(connected && !wasConnected){connectedAt=now;attempts=0;}
    wasConnected=connected;
    if(retryRequested.exchange(false)){attempts=0;lastAttempt=0;}
    if(!initialized && esp_bluedroid_get_status()==ESP_BLUEDROID_STATUS_ENABLED) {
      initialized=esp_bt_l2cap_register_callback(callback)==ESP_OK &&
                  esp_bt_l2cap_init()==ESP_OK;
    }
    const int incoming=newFd.exchange(-1);
    if(incoming>=0) {
      if(fd>=0)close(fd);
      fd=incoming;handle=openHandle.load();parser.reset();
      const bool ok=write(fd,airpods::handshake,sizeof(airpods::handshake))==sizeof(airpods::handshake) &&
        write(fd,airpods::features,sizeof(airpods::features))==sizeof(airpods::features) &&
        write(fd,airpods::notifications,sizeof(airpods::notifications))==sizeof(airpods::notifications);
      if(ok){linkState.store(1);publish(parser.state);}
      else {close(fd);fd=-1;linkState.store(0);publish(parser.state);}
    }
    if(fd>=0 && (!connected || !requested.load() || closedHandle.load()==handle)) {
      if(closedHandle.load()!=handle)close(fd);
      fd=-1;awaitingMode=0;linkState.store(0);parser.reset();publish(parser.state);
    }
    if(fd<0 && connected && requested.load() && ready.load() && !pending.load() &&
       attempts<3 && uint32_t(now-connectedAt)>=2000 &&
       (!attempts || uint32_t(now-lastAttempt)>=30000)) {
      ++attempts;lastAttempt=now;pending.store(true);
      if(esp_bt_l2cap_connect(ESP_BT_L2CAP_SEC_AUTHENTICATE|ESP_BT_L2CAP_SEC_ENCRYPT,0x1001,peer)!=ESP_OK)
        pending.store(false);
    }
    if(fd>=0) {
      // Only this low-priority task uses VFS; BT/audio/UART tasks never wait on it.
      const int mode=awaitingMode?0:pendingMode.exchange(0);uint8_t packet[11];
      if(mode && airpods::mode_command(mode,packet)) {
        if(write(fd,packet,sizeof(packet))!=sizeof(packet))Serial2.println("AIRPODS_ERROR write_failed");
        else {awaitingMode=unsigned(mode);modeSentAt=now;}
      }
      uint8_t data[256];const ssize_t bytes=read(fd,data,sizeof(data));
      if(bytes>0) {
        const auto before=parser.state.revision;parser.feed(data,size_t(bytes));
        if(before!=parser.state.revision) {
          if(parser.state.mode==int(awaitingMode))awaitingMode=0;
          publish(parser.state);
        }
      } else if(bytes<0) {
        close(fd);fd=-1;linkState.store(0);parser.reset();publish(parser.state);
      }
      if(awaitingMode && uint32_t(now-modeSentAt)>=2000) {
        // Never fill IDF's ten-packet TX queue (whose write may otherwise wait
        // 40 seconds). Only one command awaits confirmation; close on timeout.
        awaitingMode=0;pendingMode.store(0);
        Serial2.println("AIRPODS_ERROR mode_not_confirmed");
        close(fd);fd=-1;linkState.store(0);parser.reset();publish(parser.state);
      }
    } else pendingMode.store(0);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
}
void airpods_status(Print& out) {
  portENTER_CRITICAL(&statusLock);const auto state=published;portEXIT_CRITICAL(&statusLock);
  out.printf("AIRPODS connected=%d mode=%d left=%d right=%d case=%d ears=%d auto_pause=%d\n",
      linkState.load(),state.mode,state.left,state.right,state.chargeCase,state.ears,int(autoPause.load()));
}
void airpods_start(const uint8_t* mac,const char* name) {
  if(task || !mac)return;
  memcpy(peer,mac,6);
  char lower[80]{};for(size_t i=0;name && name[i] && i+1<sizeof(lower);i++)lower[i]=char(std::tolower(uint8_t(name[i])));
  requested.store(strstr(lower,"airpods")!=nullptr);
  if(settings.begin("pico-airpods",false))autoPause.store(settings.getBool("auto_pause",false));
  xTaskCreatePinnedToCore(worker,"airpods",4096,nullptr,1,&task,1);
}
bool airpods_command(const char* line,Print& out) {
  if(strncmp(line,"AIRPODS",7))return false;
  if(!strcmp(line,"AIRPODS STATUS")){airpods_status(out);return true;}
  if(!task){out.println("AIRPODS_ERROR select_device_first");return true;}
  if(!strcmp(line,"AIRPODS CONNECT")){requested.store(true);retryRequested.store(true);out.println("AIRPODS_PENDING connect");return true;}
  if(!strcmp(line,"AIRPODS DISCONNECT")){requested.store(false);out.println("AIRPODS_PENDING disconnect");return true;}
  unsigned value=0;
  if(!strncmp(line,"AIRPODS MODE ",13) && commandNumber(line+13,4,value) && value>=1) {
    if(linkState.load()!=1)out.println("AIRPODS_ERROR not_connected");
    else {pendingMode.store(value);out.println("AIRPODS_PENDING mode");}
    return true;
  }
  if(!strncmp(line,"AIRPODS AUTO_PAUSE ",19) && commandNumber(line+19,1,value)) {
    if(!settings.putBool("auto_pause",value))out.println("AIRPODS_ERROR save_failed");
    else{autoPause.store(value);airpods_status(out);}return true;
  }
  out.println("AIRPODS_ERROR invalid_command");return true;
}
void pico_codec_config(const esp_a2d_mcc_t& config) {
  if(config.type!=ESP_A2D_MCT_SBC)return;
  const auto& sbc=config.cie.sbc_info;
  codecRate.store(sbc.samp_freq==ESP_A2D_SBC_CIE_SF_44K?44100:
                  sbc.samp_freq==ESP_A2D_SBC_CIE_SF_48K?48000:
                  sbc.samp_freq==ESP_A2D_SBC_CIE_SF_32K?32000:
                  sbc.samp_freq==ESP_A2D_SBC_CIE_SF_16K?16000:-1);
  codecChannels.store(sbc.ch_mode==ESP_A2D_SBC_CIE_CH_MODE_MONO?1:2);
  codecMin.store(sbc.min_bitpool);codecMax.store(sbc.max_bitpool);
}
void pico_codec_status(Print& out) {
  out.printf("CODEC type=SBC rate=%d channels=%d bitpool_min=%d bitpool_max=%d\n",
      codecRate.load(),codecChannels.load(),codecMin.load(),codecMax.load());
}
