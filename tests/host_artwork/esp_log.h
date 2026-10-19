#pragma once
inline void artwork_test_log(const char *,const char *,...) {}
#define ESP_LOGI(tag,...) artwork_test_log(tag,__VA_ARGS__)
