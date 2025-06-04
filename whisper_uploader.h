#pragma once
#include <vector>
#include <string>

#include ".esphome/core/component.h"
#include ".esphome/components/microphone/microphone.h"
#include ".esphome/components/text_sensor/text_sensor.h"

#include "esp_http_client.h"

namespace esphome {
namespace whisper_uploader {

/**
 *  WhisperUploader
 *  ---------------
 *  • Riceve campioni PCM 16-bit via MicrophoneWriter
 *  • Costruisce un WAV mono 16 kHz in PSRAM
 *  • Alla fine del parlato invia il file via HTTP/HTTPS POST
 *  • Pubblica la trascrizione su un text_sensor
 */
class WhisperUploader : public Component,
                        public microphone::MicrophoneWriter {
 public:
  WhisperUploader(int sample_rate,
                  const std::string &host,
                  uint16_t port,
                  const std::string &path,
                  const std::string &bearer_token = "");

  // Richiamati dallo script YAML
  void start();           // comincia a bufferizzare
  void stop_and_post();   // chiude WAV e fa POST

  // ESPHome getter per il text_sensor
  text_sensor::TextSensor *result_sensor() { return result_sensor_; }

  /** MicrophoneWriter — ISR-safe  */
  void write(const int16_t *samples, size_t n_samples) override;

 protected:
  // Helpers
  void make_wav_header_();
  void post_buffer_();
  static esp_err_t http_event_handler_(esp_http_client_event_t *evt);

  // Config
  int sample_rate_;
  std::string host_;
  uint16_t port_;
  std::string path_;
  std::string token_;

  // Stato
  bool enabled_{false};
  std::vector<uint8_t> buffer_;  // PSRAM
  text_sensor::TextSensor *result_sensor_;
};

}  // namespace whisper_uploader
}  // namespace esphome