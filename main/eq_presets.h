#pragma once
#include "audio_eq.h"
#include <cstring>

namespace player {
// Gentle starting curves for Aurora's exact 11 center frequencies. Genre
// names describe a choice of tonal balance, not a correction for every mix.
struct EqFactoryPreset { const char *name; int8_t gain_db[AUDIO_EQ_BANDS]; };
inline constexpr EqFactoryPreset eq_factory_presets[]={
    {"Flat",       { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"Bass Boost", { 4, 5, 4, 2, 0, 0, 0, 0, 0, 0, 0}},
    {"Vocal",      {-3,-3,-2,-1, 1, 3, 4, 2, 0,-1,-2}},
    {"Rock",       { 3, 3, 2, 0,-1,-1, 1, 2, 3, 2, 1}},
    {"Pop",        { 1, 2, 2, 1, 1, 2, 2, 1, 1, 0,-1}},
    {"Jazz",       { 2, 2, 1, 0, 0, 1, 2, 2, 2, 1, 0}},
    {"Classical",  { 1, 1, 0, 0, 0, 0, 1, 1, 2, 2, 1}},
    {"Electronic", { 4, 4, 3, 1,-1,-1, 1, 3, 3, 2, 1}},
};
inline constexpr unsigned EQ_FACTORY_PRESET_COUNT=sizeof(eq_factory_presets)/sizeof(eq_factory_presets[0]);
inline int eq_factory_match(const audio_eq_config_t &config) {
    for(unsigned i=0;i<EQ_FACTORY_PRESET_COUNT;++i)
        if(!std::memcmp(config.gain_db,eq_factory_presets[i].gain_db,sizeof(config.gain_db)))return int(i);
    return -1;
}
}
