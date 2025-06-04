#include "whisper_uploader.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

namespace esphome {
namespace whisper_uploader {

static const char *TAG = "WhisperUploader";

WhisperUploader::WhisperUploader(int sample_rate,
                                 const std::string &host,
                                 uint16_t port,
                                 const std::string &path,
                                 const std::string &bearer_token)
    : sample_rate_(sample_rate),
      host_(host),
      port_(port),
      path_(path),
      token_(bearer_token) {
  // allochiamo il text-sensor
  result_sensor_ = new text_sensor::TextSensor();
  // riserviamo già 70 kB in PSRAM (4 s @ 16 kHz * 2 B)
  buffer_.reserve(70 * 1024);
}

void WhisperUploader::start() {
  ESP_LOGI(TAG, "Recording start");
  enabled_ = true;
  buffer_.clear();
}

void WhisperUploader::write(const int16_t *samples, size_t n_samples) {
  if (!enabled_) return;

  // Copia raw (little-endian) nella coda in PSRAM
  const uint8_t *ptr = reinterpret_cast<const uint8_t *>(samples);
  buffer_.insert(buffer_.end(), ptr, ptr + n_samples * sizeof(int16_t));
}

void WhisperUploader::stop_and_post() {
  if (!enabled_) return;
  enabled_ = false;

  ESP_LOGI(TAG, "Recording stop: %u bytes", buffer_.size());

  // Prepend header WAV RIFF PCM
  make_wav_header_();

  // POST verso Whisper-cpp
  post_buffer_();

  // libera RAM (ma tieni la capacità riservata)
  std::vector<uint8_t>().swap(buffer_);
}

void WhisperUploader::make_wav_header_() {
  const uint32_t data_size = buffer_.size();
  const uint32_t riff_size = 36 + data_size;

  uint8_t header[44];
  // RIFF chunk
  memcpy(header, "RIFF", 4);
  memcpy(header + 4, &riff_size, 4);
  memcpy(header + 8, "WAVE", 4);

  // fmt  subchunk
  memcpy(header + 12, "fmt ", 4);
  uint32_t subchunk1_size = 16;          // PCM
  uint16_t audio_format   = 1;           // PCM = 1
  uint16_t num_channels   = 1;
  uint32_t sample_rate    = sample_rate_;
  uint32_t byte_rate      = sample_rate_ * num_channels * 2;
  uint16_t block_align    = num_channels * 2;
  uint16_t bits_per_sample= 16;
  memcpy(header + 16, &subchunk1_size, 4);
  memcpy(header + 20, &audio_format,   2);
  memcpy(header + 22, &num_channels,   2);
  memcpy(header + 24, &sample_rate,    4);
  memcpy(header + 28, &byte_rate,      4);
  memcpy(header + 32, &block_align,    2);
  memcpy(header + 34, &bits_per_sample,2);

  // data subchunk
  memcpy(header + 36, "data", 4);
  memcpy(header + 40, &data_size, 4);

  // Inserisci header all’inizio del buffer
  buffer_.insert(buffer_.begin(), header, header + 44);
}

esp_err_t WhisperUploader::http_event_handler_(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
    // Accumula la risposta JSON
    std::string *resp = static_cast<std::string *>(evt->user_data);
    resp->append(reinterpret_cast<char *>(evt->data), evt->data_len);
  }
  return ESP_OK;
}

void WhisperUploader::post_buffer_() {
  std::string response;
  esp_http_client_config_t config = {};
  config.host = host_.c_str();
  config.port = port_;
  config.path = path_.c_str();
  config.event_handler = http_event_handler_;
  config.user_data = &response;
  config.transport_type = HTTP_TRANSPORT_OVER_TCP;     // LAN, no TLS
  // Se usi HTTPS, sostituisci con OVER_SSL e imposta .cert_pem

  esp_http_client_handle_t client = esp_http_client_init(&config);

  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type", "audio/wav");
  if (!token_.empty())
    esp_http_client_set_header(client, "Authorization",
                               ("Bearer " + token_).c_str());

  ESP_ERROR_CHECK(esp_http_client_open(client, buffer_.size()));
  esp_http_client_write(client,
                        reinterpret_cast<const char *>(buffer_.data()),
                        buffer_.size());
  esp_http_client_fetch_headers(client);
  esp_http_client_read_response(client, nullptr, 0);   // forza event_cb

  int status = esp_http_client_get_status_code(client);
  ESP_LOGI(TAG, "HTTP status %d", status);

  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  // parse JSON {"text":"..."} molto banalmente
  size_t pos1 = response.find("\"text\"");
  size_t pos2 = response.find_first_of(':', pos1);
  size_t quote1 = response.find_first_of('\"', pos2 + 1);
  size_t quote2 = response.find_first_of('\"', quote1 + 1);
  std::string text =
      (pos1 != std::string::npos && quote1 != std::string::npos && quote2 != std::string::npos)
          ? response.substr(quote1 + 1, quote2 - quote1 - 1)
          : "<error>";

  if (result_sensor_)
    result_sensor_->publish_state(text);

  ESP_LOGI(TAG, "Transcript: %s", text.c_str());
}

}  // namespace whisper_uploader
}  // namespace esphome