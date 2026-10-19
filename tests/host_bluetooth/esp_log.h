#pragma once
void host_bt_log(const char *tag,const char *format,...);
#define ESP_LOGI(tag,...) host_bt_log(tag,__VA_ARGS__)
