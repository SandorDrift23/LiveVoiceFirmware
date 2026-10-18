#pragma once

#include "esphome/core/automation.h"
#include "live_voice.h"

namespace esphome {
namespace live_voice {

class OnPhaseTrigger : public Trigger<std::string> {
 public:
  explicit OnPhaseTrigger(LiveVoice *parent) { parent->add_on_phase_trigger(this); }
};

}  // namespace live_voice
}  // namespace esphome
