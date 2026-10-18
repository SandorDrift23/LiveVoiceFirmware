#include "live_voice.h"
#include "automation.h"

#include "esphome/components/audio/audio.h"
#include "esphome/components/network/util.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_websocket_client.h>

namespace esphome {
namespace live_voice {

static const char *const TAG = "live_voice";

static void websocket_trampoline(void *args, esp_event_base_t, int32_t event_id, void *event_data) {
  auto *self = static_cast<LiveVoice *>(args);
  if (self != nullptr)
    self->on_ws_event(event_id, event_data);
}

void LiveVoice::setup() {
  ESP_LOGCONFIG(TAG, "Full-duplex client for %s", this->device_.c_str());
  if (!this->aec_verified_) {
    ESP_LOGW(TAG, "AEC is not verified. The microphone still stays open during playback.");
  }
  if (this->microphone_ != nullptr) {
    this->microphone_->add_data_callback([this](const std::vector<uint8_t> &data) { this->on_mic_data_(data); });
  }

  this->playback_ = static_cast<uint8_t *>(
      heap_caps_malloc(kPlaybackBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  this->mic_tx_ = static_cast<uint8_t *>(
      heap_caps_malloc(kMicBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->playback_ == nullptr || this->mic_tx_ == nullptr) {
    ESP_LOGE(TAG, "PSRAM audio buffers failed");
    this->mark_failed();
    return;
  }
  xTaskCreate(mic_sender_trampoline_, "live_mic_tx", 4096, this, 5, &this->mic_task_);

  if (this->speaker_ != nullptr) {
    audio::AudioStreamInfo info(16, 1, this->output_rate_);
    this->speaker_->set_audio_stream_info(info);
    this->speaker_->start();
  }
  this->connect_();
}

void LiveVoice::loop() {
  if (this->reconnect_requested_.exchange(false) && !this->connected_.load())
    this->schedule_reconnect_();
  if (this->resume_after_reconnect_.load() && this->resume_deadline_ms_ != 0 &&
      millis() > this->resume_deadline_ms_) {
    this->resume_after_reconnect_.store(false);
  }
  if (this->flush_requested_.exchange(false)) {
    portENTER_CRITICAL(&this->playback_mux_);
    this->playback_head_ = this->playback_tail_ = this->playback_fill_ = 0;
    this->playback_ready_ = false;
    this->has_odd_ = false;
    portEXIT_CRITICAL(&this->playback_mux_);
    if (this->speaker_ != nullptr) {
      this->speaker_->stop();
      audio::AudioStreamInfo info(16, 1, this->output_rate_);
      this->speaker_->set_audio_stream_info(info);
      this->speaker_->start();
    }
  }
  if (this->speaker_ == nullptr || this->playback_ == nullptr)
    return;
  // One pass is only ~11 ms. A spoken reply arrives in a burst, so keep
  // handing audio to the speaker until it is caught up or its queue is full.
  for (int pass = 0; pass < 40 && this->play_available_(); pass++) {
  }
}

bool LiveVoice::play_available_() {
  portENTER_CRITICAL(&this->playback_mux_);
  size_t head = this->playback_head_;
  size_t fill = this->playback_fill_;
  portEXIT_CRITICAL(&this->playback_mux_);
  if (!this->playback_ready_) {
    if (fill < kPrebufferBytes)
      return false;
    this->playback_ready_ = true;
  }
  if (fill == 0)
    return false;
  size_t contiguous = std::min(fill, kPlaybackBytes - head);
  contiguous = std::min(contiguous, static_cast<size_t>(1024));
  contiguous -= contiguous % 2;
  if (contiguous == 0)
    return false;
  const int32_t gain = this->volume_q15_.load();
  size_t accepted = 0;
  if (gain >= 32767) {
    accepted = this->speaker_->play(this->playback_ + head, contiguous);
  } else {
    int16_t scaled[512];
    const size_t samples = contiguous / 2;
    const auto *input = reinterpret_cast<const int16_t *>(this->playback_ + head);
    for (size_t index = 0; index < samples; index++) {
      int32_t sample = (static_cast<int32_t>(input[index]) * gain) >> 15;
      if (sample > 32767)
        sample = 32767;
      else if (sample < -32768)
        sample = -32768;
      scaled[index] = static_cast<int16_t>(sample);
    }
    accepted = this->speaker_->play(reinterpret_cast<uint8_t *>(scaled), contiguous);
  }
  if (accepted < 2)
    return false;
  accepted -= accepted % 2;
  portENTER_CRITICAL(&this->playback_mux_);
  this->playback_head_ = (this->playback_head_ + accepted) % kPlaybackBytes;
  this->playback_fill_ -= accepted;
  portEXIT_CRITICAL(&this->playback_mux_);
  return true;
}

void LiveVoice::set_volume(float volume) {
  if (volume < 0.0f)
    volume = 0.0f;
  else if (volume > 1.0f)
    volume = 1.0f;
  uint16_t gain = volume >= 1.0f ? 32767 : static_cast<uint16_t>(volume * 32767.0f);
  this->volume_q15_.store(gain);
  ESP_LOGI(TAG, "Speaker volume %.0f%%", volume * 100.0f);
}

void LiveVoice::adjust_volume(float delta) {
  this->set_volume(this->volume() + delta);
}

void LiveVoice::start_session() {
  if (!this->connected_) {
    ESP_LOGW(TAG, "Wake ignored: gateway is not connected");
    return;
  }
  this->resume_after_reconnect_.store(false);
  this->streaming_.store(true);
  this->send_text_("{\"type\":\"wake\"}");
  this->set_phase_("listening");
  ESP_LOGI(TAG, "Conversation open. Microphone stays on during playback.");
}

void LiveVoice::send_interrupt() {
  this->flush_requested_.store(true);
  if (this->connected_)
    this->send_text_("{\"type\":\"interrupt\"}");
  this->set_phase_("listening");
}

void LiveVoice::end_session() {
  this->resume_after_reconnect_.store(false);
  this->streaming_.store(false);
  this->flush_requested_.store(true);
  if (this->connected_)
    this->send_text_("{\"type\":\"end\"}");
  this->set_phase_("idle");
  ESP_LOGI(TAG, "Conversation ended");
}

void LiveVoice::connect_() {
  if (!network::is_connected()) {
    this->schedule_reconnect_after_(1000);
    return;
  }
  if (this->ws_handle_ != nullptr) {
    auto handle = static_cast<esp_websocket_client_handle_t>(this->ws_handle_);
    if (esp_websocket_client_is_connected(handle))
      return;
    // start() returns immediately. stop() would block this loop until the
    // socket timeout ends, and that trips the task watchdog.
    if (esp_websocket_client_start(handle) != ESP_OK)
      this->schedule_reconnect_();
    return;
  }
  esp_websocket_client_config_t config = {};
  config.uri = this->url_.c_str();
  config.disable_auto_reconnect = true;
  // One assistant frame has to fit even if the gateway sends a short burst.
  // The client aborts the socket when a microphone write cannot finish.
  config.buffer_size = 16384;
  config.task_stack = 10240;
  config.ping_interval_sec = 30;
  config.pingpong_timeout_sec = 60;
  config.network_timeout_ms = 4000;
  auto handle = esp_websocket_client_init(&config);
  if (handle == nullptr) {
    this->schedule_reconnect_();
    return;
  }
  this->ws_handle_ = handle;
  esp_websocket_register_events(handle, WEBSOCKET_EVENT_ANY, websocket_trampoline, this);
  ESP_LOGI(TAG, "Connecting to %s", this->url_.c_str());
  if (esp_websocket_client_start(handle) != ESP_OK)
    this->schedule_reconnect_();
}

void LiveVoice::schedule_reconnect_() {
  if (this->reconnect_pending_)
    return;
  uint32_t delay = this->reconnect_delay_ms_;
  this->reconnect_delay_ms_ = std::min<uint32_t>(this->reconnect_delay_ms_ * 2, 10000);
  this->schedule_reconnect_after_(delay);
}

void LiveVoice::schedule_reconnect_after_(uint32_t delay_ms) {
  if (this->reconnect_pending_)
    return;
  this->reconnect_pending_ = true;
  this->set_timeout(delay_ms, [this]() {
    this->reconnect_pending_ = false;
    this->connect_();
  });
}

void LiveVoice::send_text_(const std::string &payload) {
  auto handle = static_cast<esp_websocket_client_handle_t>(this->ws_handle_);
  if (!this->connected_.load() || handle == nullptr)
    return;
  esp_websocket_client_send_text(handle, payload.c_str(), static_cast<int>(payload.size()), pdMS_TO_TICKS(1000));
}

void LiveVoice::send_hello_() {
  char payload[192];
  snprintf(payload, sizeof(payload),
           "{\"type\":\"hello\",\"protocol\":1,\"device\":\"%s\",\"input_rate\":%u,"
           "\"output_rate\":%u,\"duplex\":true,\"aec_verified\":%s}",
           this->device_.c_str(), static_cast<unsigned>(this->input_rate_),
           static_cast<unsigned>(this->output_rate_), this->aec_verified_ ? "true" : "false");
  this->send_text_(payload);
}

void LiveVoice::on_ws_event(int32_t event_id, void *event_data) {
  if (event_id == WEBSOCKET_EVENT_CONNECTED) {
    this->connected_.store(true);
    this->reconnect_delay_ms_ = 1000;
    if (this->resume_after_reconnect_.load())
      this->resume_deadline_ms_ = millis() + 3000;
    ESP_LOGI(TAG, "Gateway connected");
    this->send_hello_();
    return;
  }
  if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_CLOSED ||
      event_id == WEBSOCKET_EVENT_ERROR) {
    // The websocket task must not touch ESPHome timers. loop() reconnects.
    if (this->connected_.exchange(false)) {
      if (this->streaming_.exchange(false))
        this->resume_after_reconnect_.store(true);
      // Drop audio captured while the socket was dying. Sending that backlog
      // on the new socket overflows it and the connection drops again.
      portENTER_CRITICAL(&this->mic_mux_);
      this->mic_head_ = this->mic_tail_ = this->mic_fill_ = 0;
      portEXIT_CRITICAL(&this->mic_mux_);
      ESP_LOGW(TAG, "Gateway disconnected (event %d)", static_cast<int>(event_id));
    }
    this->reconnect_requested_.store(true);
    return;
  }
  if (event_id != WEBSOCKET_EVENT_DATA || event_data == nullptr)
    return;
  auto *event = static_cast<esp_websocket_event_data_t *>(event_data);
  if (event->op_code == 0x01) {
    this->text_fragment_.append(reinterpret_cast<const char *>(event->data_ptr), event->data_len);
    if (event->payload_offset + event->data_len >= event->payload_len) {
      this->handle_text_(this->text_fragment_);
      this->text_fragment_.clear();
    }
    return;
  }
  if (event->op_code == 0x02)
    this->handle_binary_(reinterpret_cast<const uint8_t *>(event->data_ptr), static_cast<size_t>(event->data_len));
}

void LiveVoice::handle_text_(const std::string &payload) {
  if (payload.find("\"type\":\"flush\"") != std::string::npos) {
    this->flush_requested_.store(true);
    return;
  }
  if (payload.find("\"phase\":\"idle\"") != std::string::npos) {
    this->resume_after_reconnect_.store(false);
    this->streaming_.store(false);
    this->flush_requested_.store(true);
    this->defer([this]() { this->set_phase_("idle"); });
    return;
  }
  const bool listening = payload.find("\"phase\":\"listening\"") != std::string::npos;
  const bool speaking = payload.find("\"phase\":\"speaking\"") != std::string::npos;
  if (listening || speaking) {
    if (this->resume_after_reconnect_.exchange(false)) {
      this->streaming_.store(true);
      ESP_LOGI(TAG, "Conversation resumed after reconnect");
    }
    this->defer([this, listening]() { this->set_phase_(listening ? "listening" : "speaking"); });
  }
}

void LiveVoice::handle_binary_(const uint8_t *data, size_t length) {
  if (this->playback_ == nullptr || length == 0)
    return;
  const uint8_t *pcm = data;
  size_t remaining = length;
  if (this->has_odd_ && remaining > 0) {
    uint8_t pair[2] = {this->odd_byte_, pcm[0]};
    this->has_odd_ = false;
    this->write_playback_(pair, 2);
    pcm++;
    remaining--;
  }
  if (remaining % 2 != 0) {
    this->has_odd_ = true;
    this->odd_byte_ = pcm[remaining - 1];
    remaining--;
  }
  if (remaining > 0)
    this->write_playback_(pcm, remaining);
}

void LiveVoice::write_playback_(const uint8_t *data, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    portENTER_CRITICAL(&this->playback_mux_);
    size_t free_bytes = kPlaybackBytes - this->playback_fill_;
    size_t chunk = std::min(length - offset, free_bytes);
    chunk = std::min(chunk, static_cast<size_t>(256));
    if (chunk == 0) {
      portEXIT_CRITICAL(&this->playback_mux_);
      uint32_t now = millis();
      if (now - this->overflow_log_ms_ > 2000) {
        this->overflow_log_ms_ = now;
        ESP_LOGW(TAG, "Playback ring overflow");
      }
      return;
    }
    size_t tail = this->playback_tail_;
    size_t first = std::min(chunk, kPlaybackBytes - tail);
    std::memcpy(this->playback_ + tail, data + offset, first);
    if (first < chunk)
      std::memcpy(this->playback_, data + offset + first, chunk - first);
    this->playback_tail_ = (tail + chunk) % kPlaybackBytes;
    this->playback_fill_ += chunk;
    portEXIT_CRITICAL(&this->playback_mux_);
    offset += chunk;
  }
}

void LiveVoice::on_mic_data_(const std::vector<uint8_t> &samples) {
  if (!this->streaming_.load() || samples.size() < 8)
    return;
  const auto *input = reinterpret_cast<const int32_t *>(samples.data());
  size_t frames = (samples.size() / 4) / 2;
  this->mono_.resize(frames);
  for (size_t index = 0; index < frames; index++)
    this->mono_[index] = static_cast<int16_t>(input[index * 2 + this->mic_channel_] >> 16);
  this->mic_queue_push_(reinterpret_cast<const uint8_t *>(this->mono_.data()), frames * sizeof(int16_t));
}

bool LiveVoice::mic_queue_push_(const uint8_t *data, size_t length) {
  if (this->mic_tx_ == nullptr || length == 0)
    return false;
  portENTER_CRITICAL(&this->mic_mux_);
  if (length > kMicBytes - this->mic_fill_) {
    portEXIT_CRITICAL(&this->mic_mux_);
    ESP_LOGW(TAG, "Microphone ring overflow");
    return false;
  }
  size_t tail = this->mic_tail_;
  size_t first = std::min(length, kMicBytes - tail);
  std::memcpy(this->mic_tx_ + tail, data, first);
  if (first < length)
    std::memcpy(this->mic_tx_, data + first, length - first);
  this->mic_tail_ = (tail + length) % kMicBytes;
  this->mic_fill_ += length;
  portEXIT_CRITICAL(&this->mic_mux_);
  if (this->mic_task_ != nullptr)
    xTaskNotifyGive(this->mic_task_);
  return true;
}

void LiveVoice::mic_sender_trampoline_(void *arg) {
  static_cast<LiveVoice *>(arg)->mic_sender_loop_();
}

void LiveVoice::mic_sender_loop_() {
  uint8_t packet[1280];
  while (true) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    if (!this->connected_.load() || !this->streaming_.load() || this->ws_handle_ == nullptr)
      continue;
    portENTER_CRITICAL(&this->mic_mux_);
    size_t available = std::min(this->mic_fill_, sizeof(packet));
    size_t head = this->mic_head_;
    if (available > 0) {
      size_t first = std::min(available, kMicBytes - head);
      std::memcpy(packet, this->mic_tx_ + head, first);
      if (first < available)
        std::memcpy(packet + first, this->mic_tx_, available - first);
    }
    portEXIT_CRITICAL(&this->mic_mux_);
    if (available == 0)
      continue;
    int sent = esp_websocket_client_send_bin(static_cast<esp_websocket_client_handle_t>(this->ws_handle_),
                                              reinterpret_cast<const char *>(packet), static_cast<int>(available),
                                              pdMS_TO_TICKS(1000));
    if (sent <= 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    portENTER_CRITICAL(&this->mic_mux_);
    size_t consumed = std::min(static_cast<size_t>(sent), this->mic_fill_);
    this->mic_head_ = (this->mic_head_ + consumed) % kMicBytes;
    this->mic_fill_ -= consumed;
    portEXIT_CRITICAL(&this->mic_mux_);
  }
}

void LiveVoice::set_phase_(const std::string &phase) {
  this->phase_ = phase;
  for (auto *trigger : this->phase_triggers_)
    trigger->trigger(phase);
}

}  // namespace live_voice
}  // namespace esphome
