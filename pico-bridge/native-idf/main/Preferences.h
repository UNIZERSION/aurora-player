#pragma once
#include "Arduino.h"
#include "nvs.h"
class Preferences {
 public:
  bool begin(const char* name,bool readonly) {
    return nvs_open(name,readonly?NVS_READONLY:NVS_READWRITE,&handle_)==ESP_OK;
  }
  size_t getBytesLength(const char* key) {
    size_t n=0;return nvs_get_blob(handle_,key,nullptr,&n)==ESP_OK?n:0;
  }
  size_t getBytes(const char* key,void* data,size_t capacity) {
    size_t n=capacity;return nvs_get_blob(handle_,key,data,&n)==ESP_OK?n:0;
  }
  size_t putBytes(const char* key,const void* data,size_t n) {
    return nvs_set_blob(handle_,key,data,n)==ESP_OK && nvs_commit(handle_)==ESP_OK?n:0;
  }
  bool remove(const char* key) {
    return nvs_erase_key(handle_,key)==ESP_OK && nvs_commit(handle_)==ESP_OK;
  }
  uint8_t getUChar(const char* key,uint8_t fallback=0) {
    uint8_t n=fallback; nvs_get_u8(handle_,key,&n);return n;
  }
  size_t putUChar(const char* key,uint8_t n) {
    return nvs_set_u8(handle_,key,n)==ESP_OK && nvs_commit(handle_)==ESP_OK?1:0;
  }
  bool getBool(const char* key,bool fallback=false) { return getUChar(key,fallback); }
  size_t putBool(const char* key,bool n) { return putUChar(key,n); }
  String getString(const char* key,const char* fallback) {
    char value[256];size_t n=sizeof(value);
    return nvs_get_str(handle_,key,value,&n)==ESP_OK?String(value):String(fallback);
  }
  size_t putString(const char* key,const char* n) {
    return nvs_set_str(handle_,key,n)==ESP_OK && nvs_commit(handle_)==ESP_OK?strlen(n):0;
  }
 private: nvs_handle_t handle_=0;
};
