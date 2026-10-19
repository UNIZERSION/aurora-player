#include "eq_presets.h"
// Test-only bridge: the C engine fixture consumes the actual production table.
extern "C" unsigned host_eq_factory_count(){return player::EQ_FACTORY_PRESET_COUNT;}
extern "C" void host_eq_factory_config(unsigned index,audio_eq_config_t *config){
    *config={};config->enabled=1;
    if(index<player::EQ_FACTORY_PRESET_COUNT)
        std::memcpy(config->gain_db,player::eq_factory_presets[index].gain_db,sizeof config->gain_db);
}
