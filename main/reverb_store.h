#pragma once
#include "audio_fx.h"
#include <cstdint>
#include <string>

namespace player {
/* Independent of the released Settings ABI. Call from the controller, outside
 * LVGL/audio locks. A/B records retain the last complete saved configuration;
 * do not write while a slider is being dragged (use the normal save debounce). */
class ReverbStore {
public:
    explicit ReverbStore(std::string directory);
    bool load(audio_reverb_config_t &out);
    bool save(const audio_reverb_config_t &config);
    uint64_t sequence() const { return sequence_; }
private:
    std::string directory_;
    uint64_t sequence_=0;
};
}
