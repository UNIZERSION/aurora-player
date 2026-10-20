#pragma once
#ifdef PICO_AIRPODS_HOST_TEST
#include "airpods_mocks.hpp"
#else
#include "Arduino.h"
#endif
void airpods_start(const uint8_t* mac,const char* name);
bool airpods_command(const char* line,Print& out);
void airpods_status(Print& out);
void pico_codec_status(Print& out);
