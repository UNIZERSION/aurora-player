#pragma once
#include "mocks/Mock.hpp"
#include <atomic>
using TaskHandle_t=void*;
using ssize_t=std::ptrdiff_t;
constexpr int ESP_BLUEDROID_STATUS_ENABLED=2,ESP_BT_L2CAP_SUCCESS=0;
constexpr unsigned ESP_BT_L2CAP_SEC_AUTHENTICATE=0x12,ESP_BT_L2CAP_SEC_ENCRYPT=0x24;
enum esp_bt_l2cap_cb_event_t {ESP_BT_L2CAP_INIT_EVT,ESP_BT_L2CAP_VFS_REGISTER_EVT,ESP_BT_L2CAP_OPEN_EVT,ESP_BT_L2CAP_CLOSE_EVT,ESP_BT_L2CAP_CL_INIT_EVT};
struct esp_bt_l2cap_cb_param_t {
 struct {int status=0;} init,vfs_register,cl_init;
 struct {int status=0;uint32_t handle=0;int fd=-1;uint8_t rem_bda[6]{};} open;
 struct {uint32_t handle=0;} close;
};
using L2Callback=void(*)(esp_bt_l2cap_cb_event_t,esp_bt_l2cap_cb_param_t*);
inline L2Callback l2callback=nullptr;
inline std::vector<std::vector<uint8_t>> sentPackets;
inline std::deque<std::vector<uint8_t>> receivedPackets;
inline unsigned connects=0,closes=0;
inline uint8_t connectedPeer[6]{};
inline int esp_bluedroid_get_status(){return ESP_BLUEDROID_STATUS_ENABLED;}
inline int esp_bt_l2cap_register_callback(L2Callback callback){l2callback=callback;return ESP_OK;}
inline int esp_bt_l2cap_vfs_register(){esp_bt_l2cap_cb_param_t p;l2callback(ESP_BT_L2CAP_VFS_REGISTER_EVT,&p);return ESP_OK;}
inline int esp_bt_l2cap_init(){esp_bt_l2cap_cb_param_t p;l2callback(ESP_BT_L2CAP_INIT_EVT,&p);return ESP_OK;}
inline int esp_bt_l2cap_connect(unsigned security,unsigned psm,uint8_t* peer) {
 if(security!=(ESP_BT_L2CAP_SEC_AUTHENTICATE|ESP_BT_L2CAP_SEC_ENCRYPT)||psm!=0x1001)throw std::runtime_error("wrong L2CAP settings");
 ++connects;memcpy(connectedPeer,peer,6);
 esp_bt_l2cap_cb_param_t p;p.open.fd=7;p.open.handle=42;memcpy(p.open.rem_bda,peer,6);
 l2callback(ESP_BT_L2CAP_OPEN_EVT,&p);return ESP_OK;
}
inline ssize_t fake_write(int fd,const void* data,size_t size) {
 if(fd!=7)throw std::runtime_error("wrong fd");
 const auto* bytes=static_cast<const uint8_t*>(data);sentPackets.emplace_back(bytes,bytes+size);return ssize_t(size);
}
inline ssize_t fake_read(int fd,void* data,size_t size) {
 if(fd!=7)throw std::runtime_error("wrong fd");
 if(receivedPackets.empty())return 0;
 auto& bytes=receivedPackets.front();size_t n=std::min(size,bytes.size());
 memcpy(data,bytes.data(),n);bytes.erase(bytes.begin(),bytes.begin()+n);if(bytes.empty())receivedPackets.pop_front();return ssize_t(n);
}
inline int fake_close(int fd){if(fd!=7)throw std::runtime_error("wrong close fd");++closes;return 0;}
inline std::function<void()> onStep;
inline void fake_delay(unsigned ms){mock_ms+=ms;if(onStep)onStep();}
constexpr int ESP_A2D_MCT_SBC=0,ESP_A2D_SBC_CIE_SF_44K=2,ESP_A2D_SBC_CIE_SF_48K=1,ESP_A2D_SBC_CIE_SF_32K=4,ESP_A2D_SBC_CIE_SF_16K=8,ESP_A2D_SBC_CIE_CH_MODE_MONO=8;
struct esp_a2d_mcc_t {
 int type=0;struct {struct {int samp_freq=2,ch_mode=1,min_bitpool=2,max_bitpool=53;} sbc_info;} cie;
};
#define write(fd,data,size) fake_write(fd,data,size)
#define read(fd,data,size) fake_read(fd,data,size)
#define close(fd) fake_close(fd)
#define vTaskDelay fake_delay
