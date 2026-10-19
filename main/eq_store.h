#pragma once
#include "audio_eq.h"
#include "audio_level_policy.h"
#include <cstdint>
#include <string>

namespace player {
constexpr unsigned EQ_PRESET_COUNT=4;
constexpr int8_t EQ_DEFAULT_PREAMP_DB=AUDIO_MASTER_GAIN_DB;
struct EqBank {
    audio_eq_config_t current{};
    audio_eq_config_t presets[EQ_PRESET_COUNT]{};
    uint8_t selected=0;
    uint8_t saved_mask=0;
    EqBank() {
        current.enabled=1;current.preamp_db=EQ_DEFAULT_PREAMP_DB;
        for(auto &preset:presets){preset.enabled=1;preset.preamp_db=EQ_DEFAULT_PREAMP_DB;}
    }
};

/* Separate from released Settings ABI. Four user slots and the working curve
 * are saved together. Caller serializes operations, owns its dirty flag, and
 * performs all filesystem work outside LVGL/audio locks.
 *
 * Missing/corrupt files leave the supplied bank unchanged. Save validates the
 * complete bank, writes the older A/B slot, flushes/syncs/closes and verifies
 * exact readback. On failure the previous valid slot remains untouched; a
 * power cut may recover either complete old or complete new state.
 */
class EqStore {
public:
    explicit EqStore(std::string directory);
    bool load(EqBank &out);
    bool save(const EqBank &bank);
    uint64_t sequence() const { return sequence_; }
private:
    std::string directory_;
    uint64_t sequence_=0;
};
}
