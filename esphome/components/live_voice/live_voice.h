#pragma once

#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/task.h>

namespace esphome {
namespace live_voice {

class OnPhaseTrigger;

// Full-duplex PCM client. The microphone stays open for the whole
// conversation, including while assistant audio is playing.
class LiveVoice : public Component {
 public:
  void set_url(const std::string &url) { this->url_ = url; }
  void set_device(const std::string &device) { this->device_ = device; }
  void set_microphone(microphone::Microphone *microphone) { this->microphone_ = microphone; }
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
  void set_mic_channel(uint8_t channel) { this->mic_channel_ = channel; }
  void set_input_rate(uint32_t rate) { this->input_rate_ = rate; }
  void set_output_rate(uint32_t rate) { this->output_rate_ = rate; }
  void set_aec_verified(bool verified) { this->aec_verified_ = verified; }
  void add_on_phase_trigger(OnPhaseTrigger *trigger) { this->phase_triggers_.push_back(trigger); }

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  bool is_connected() const { return this->connected_; }
  bool is_streaming() const { return this->streaming_.load(); }
  float volume() const { return this->volume_q15_.load() / 32768.0f; }
  void set_volume(float volume);
  void adjust_volume(float delta);
  void start_session();
  void send_interrupt();
  void end_session();
  void on_ws_event(int32_t event_id, void *event_data);

 protected:
  void connect_();
  void schedule_reconnect_();
  void schedule_reconnect_after_(uint32_t delay_ms);
  void send_text_(const std::string &payload);
  void send_hello_();
  void on_mic_data_(const std::vector<uint8_t> &samples);
  bool mic_queue_push_(const uint8_t *data, size_t length);
  static void mic_sender_trampoline_(void *arg);
  void mic_sender_loop_();
  void handle_text_(const std::string &payload);
  void handle_binary_(const uint8_t *data, size_t length);
  void write_playback_(const uint8_t *data, size_t length);
  bool play_available_();
  void set_phase_(const std::string &phase);

  std::string url_;
  std::string device_{"voice_pe"};
  microphone::Microphone *microphone_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  uint8_t mic_channel_{0};
  uint32_t input_rate_{16000};
  uint32_t output_rate_{24000};
  bool aec_verified_{false};

  void *ws_handle_{nullptr};
  std::atomic<bool> connected_{false};
  bool reconnect_pending_{false};
  std::atomic<bool> reconnect_requested_{false};
  uint32_t reconnect_delay_ms_{1000};
  std::atomic<bool> streaming_{false};
  std::atomic<bool> resume_after_reconnect_{false};
  uint32_t resume_deadline_ms_{0};

  std::vector<int16_t> mono_;
  // Full scale. Voice PE volume is the AIC3204 gain, not a second PCM cut.
  std::atomic<uint16_t> volume_q15_{32767};
  bool has_odd_{false};
  uint8_t odd_byte_{0};
  std::string text_fragment_;
  std::vector<OnPhaseTrigger *> phase_triggers_;
  std::string phase_{"idle"};

  // Three seconds at 48 kHz, six at the Voice PE rate. Holds a search reply
  // that arrives faster than the speaker can play it.
  static constexpr size_t kPlaybackBytes = 48000 * 2 * 3;
  static constexpr size_t kMicBytes = 16000 * 2 * 2;
  uint8_t *playback_{nullptr};
  uint8_t *mic_tx_{nullptr};
  size_t playback_head_{0};
  size_t playback_tail_{0};
  size_t playback_fill_{0};
  size_t mic_head_{0};
  size_t mic_tail_{0};
  size_t mic_fill_{0};
  portMUX_TYPE playback_mux_ = portMUX_INITIALIZER_UNLOCKED;
  portMUX_TYPE mic_mux_ = portMUX_INITIALIZER_UNLOCKED;
  TaskHandle_t mic_task_{nullptr};
  std::atomic<bool> flush_requested_{false};
  bool playback_ready_{false};
  uint32_t overflow_log_ms_{0};
  static constexpr size_t kPrebufferBytes = 24000 / 1000 * 2 * 60;
};

}  // namespace live_voice
}  // namespace esphome
