#include "sound_cache.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <vector>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_spiffs.h"
#include "esphome/components/audio/audio.h"
#include "esphome/core/log.h"

namespace esphome::sound_cache {

static const char *const TAG = "sound_cache";

static uint16_t le16(const uint8_t *p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
static uint32_t le32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

void SoundCache::setup() {
  esp_vfs_spiffs_conf_t conf{};
  conf.base_path = mount_path_.c_str();
  conf.partition_label = partition_label_.c_str();
  conf.format_if_mount_failed = true;
  conf.max_files = 8;
  esp_err_t err = esp_vfs_spiffs_register(&conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
    mark_failed();
    return;
  }
  load_index_();
  ESP_LOGI(TAG, "SPIFFS ready, free=%llu bytes", static_cast<unsigned long long>(free_bytes_()));
}

void SoundCache::dump_config() {
  ESP_LOGCONFIG(TAG, "Sound cache:");
  ESP_LOGCONFIG(TAG, "  Base URL: %s", base_url_.c_str());
  ESP_LOGCONFIG(TAG, "  Partition: %s", partition_label_.c_str());
  ESP_LOGCONFIG(TAG, "  Reserve: %lu bytes", static_cast<unsigned long>(reserve_bytes_));
  ESP_LOGCONFIG(TAG, "  Maximum file: %lu bytes", static_cast<unsigned long>(max_file_size_));
}

bool SoundCache::sanitize_name_(const std::string &input, std::string &name) const {
  name = input;
  const auto q = name.find('?');
  if (q != std::string::npos) name.resize(q);
  const auto slash = name.find_last_of('/');
  if (slash != std::string::npos) name = name.substr(slash + 1);
  if (name.size() < 5 || name.size() > 80 || name.substr(name.size() - 4) != ".wav") return false;
  for (char c : name)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) return false;
  return name != ".wav" && name.find("..") == std::string::npos;
}

void SoundCache::play(const std::string &input, float volume) {
  std::string name;
  if (!sanitize_name_(input, name)) {
    ESP_LOGE(TAG, "Rejected sound name: %s", input.c_str());
    return;
  }
  cancel_ = true;
  portENTER_CRITICAL(&request_lock_);
  pending_name_ = name;
  pending_volume_ = std::max(0.0f, std::min(1.0f, volume));
  portEXIT_CRITICAL(&request_lock_);
  if (!busy_.exchange(true)) {
    xTaskCreatePinnedToCore(task_entry_, "sound_cache", 8192, this, 3, nullptr, 0);
  }
}

void SoundCache::task_entry_(void *param) {
  static_cast<SoundCache *>(param)->task_();
  vTaskDelete(nullptr);
}

void SoundCache::task_() {
  while (true) {
    std::string name;
    float volume;
    portENTER_CRITICAL(&request_lock_);
    name.swap(pending_name_);
    volume = pending_volume_;
    portEXIT_CRITICAL(&request_lock_);
    if (name.empty()) break;
    cancel_ = false;

    std::string path;
    if (ensure_cached_(name, path)) {
      touch_(name);
      save_index_();
      play_file_(path, volume);
    }
  }
  busy_ = false;
  // Close the small race between checking the queue and clearing busy_.
  portENTER_CRITICAL(&request_lock_);
  const bool restart = !pending_name_.empty();
  portEXIT_CRITICAL(&request_lock_);
  if (restart && !busy_.exchange(true))
    xTaskCreatePinnedToCore(task_entry_, "sound_cache", 8192, this, 3, nullptr, 0);
}

bool SoundCache::ensure_cached_(const std::string &name, std::string &path) {
  path = mount_path_ + "/" + name;
  uint32_t offset = 0, size = 0;
  if (validate_wav_(path, offset, size)) {
    ESP_LOGI(TAG, "Cache hit: %s", name.c_str());
    return true;
  }
  std::remove(path.c_str());
  const std::string tmp = path + ".part";
  std::remove(tmp.c_str());
  if (!download_(name, tmp)) {
    std::remove(tmp.c_str());
    return false;
  }
  struct stat st{};
  if (stat(tmp.c_str(), &st) != 0 || st.st_size <= 44 || static_cast<uint64_t>(st.st_size) > max_file_size_) {
    ESP_LOGE(TAG, "Invalid download size for %s", name.c_str());
    std::remove(tmp.c_str());
    return false;
  }
  if (!validate_wav_(tmp, offset, size)) {
    ESP_LOGE(TAG, "Downloaded file is not 22050 Hz/16-bit/mono PCM WAV: %s", name.c_str());
    std::remove(tmp.c_str());
    return false;
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    ESP_LOGE(TAG, "Atomic cache commit failed: %s", std::strerror(errno));
    std::remove(tmp.c_str());
    return false;
  }
  ESP_LOGI(TAG, "Cached complete file: %s", name.c_str());
  return true;
}

bool SoundCache::download_(const std::string &name, const std::string &tmp_path) {
  std::string url = base_url_;
  if (!url.empty() && url.back() != '/') url.push_back('/');
  url += name;
  esp_http_client_config_t cfg{};
  cfg.url = url.c_str();
  cfg.timeout_ms = 20000;
  cfg.buffer_size = 8192;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) return false;
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return false;
  }
  const int64_t announced = esp_http_client_fetch_headers(client);
  const int status = esp_http_client_get_status_code(client);
  if (status != 200 || announced > static_cast<int64_t>(max_file_size_)) {
    ESP_LOGE(TAG, "HTTP %d, size=%lld for %s", status, static_cast<long long>(announced), name.c_str());
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }
  if (announced > 0 && !ensure_space_(announced + reserve_bytes_, name)) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }
  FILE *out = std::fopen(tmp_path.c_str(), "wb");
  if (out == nullptr) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }
  std::vector<uint8_t> buffer(8192);
  uint64_t total = 0;
  bool ok = true;
  while (true) {
    const int got = esp_http_client_read(client, reinterpret_cast<char *>(buffer.data()), buffer.size());
    if (got < 0) { ok = false; break; }
    if (got == 0) break;
    total += got;
    if (total > max_file_size_ || std::fwrite(buffer.data(), 1, got, out) != static_cast<size_t>(got)) {
      ok = false;
      break;
    }
  }
  std::fflush(out);
  std::fclose(out);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  if (announced > 0 && total != static_cast<uint64_t>(announced)) ok = false;
  if (!ok) ESP_LOGE(TAG, "Incomplete download of %s (%llu bytes)", name.c_str(), static_cast<unsigned long long>(total));
  return ok;
}

bool SoundCache::validate_wav_(const std::string &path, uint32_t &data_offset, uint32_t &data_size) {
  FILE *f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  uint8_t hdr[12];
  bool ok = std::fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && !std::memcmp(hdr, "RIFF", 4) &&
            !std::memcmp(hdr + 8, "WAVE", 4);
  bool fmt_ok = false, data_ok = false;
  while (ok && !data_ok) {
    uint8_t chunk[8];
    if (std::fread(chunk, 1, 8, f) != 8) break;
    uint32_t len = le32(chunk + 4);
    if (!std::memcmp(chunk, "fmt ", 4)) {
      if (len < 16 || len > 64) break;
      std::vector<uint8_t> fmt(len);
      if (std::fread(fmt.data(), 1, len, f) != len) break;
      fmt_ok = le16(fmt.data()) == 1 && le16(fmt.data() + 2) == 1 && le32(fmt.data() + 4) == 22050 &&
               le16(fmt.data() + 14) == 16;
    } else if (!std::memcmp(chunk, "data", 4)) {
      data_offset = std::ftell(f);
      data_size = len;
      data_ok = fmt_ok && data_size > 0;
      break;
    } else {
      if (std::fseek(f, len + (len & 1U), SEEK_CUR) != 0) break;
    }
  }
  std::fclose(f);
  return ok && data_ok;
}

bool SoundCache::play_file_(const std::string &path, float volume) {
  uint32_t offset = 0, remaining = 0;
  if (!validate_wav_(path, offset, remaining)) return false;
  FILE *f = std::fopen(path.c_str(), "rb");
  if (f == nullptr || std::fseek(f, offset, SEEK_SET) != 0) {
    if (f) std::fclose(f);
    return false;
  }
  speaker_->stop();
  while (!speaker_->is_stopped()) vTaskDelay(pdMS_TO_TICKS(10));
  speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, 22050));
  speaker_->start();
  std::vector<int16_t> samples(2048);
  while (remaining > 0 && !cancel_) {
    const size_t want = std::min<size_t>(remaining, samples.size() * sizeof(int16_t));
    const size_t got = std::fread(samples.data(), 1, want, f);
    if (got == 0) break;
    const size_t count = got / sizeof(int16_t);
    for (size_t i = 0; i < count; i++) {
      int32_t scaled = static_cast<int32_t>(samples[i] * volume);
      samples[i] = static_cast<int16_t>(std::max<int32_t>(-32768, std::min<int32_t>(32767, scaled)));
    }
    size_t written = 0;
    while (written < got && !cancel_) {
      written += speaker_->play(reinterpret_cast<uint8_t *>(samples.data()) + written, got - written,
                                pdMS_TO_TICKS(100));
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    remaining -= got;
  }
  std::fclose(f);
  if (cancel_)
    speaker_->stop();
  else
    speaker_->finish();
  ESP_LOGI(TAG, "Local playback finished: %s", path.c_str());
  return remaining == 0 && !cancel_;
}

uint64_t SoundCache::free_bytes_() const {
  size_t total = 0;
  size_t used = 0;
  if (esp_spiffs_info(partition_label_.c_str(), &total, &used) != ESP_OK || used > total) return 0;
  return total - used;
}

bool SoundCache::ensure_space_(uint64_t needed, const std::string &protected_name) {
  while (free_bytes_() < needed) {
    auto oldest = lru_.end();
    for (auto it = lru_.begin(); it != lru_.end(); ++it)
      if (it->first != protected_name && (oldest == lru_.end() || it->second < oldest->second)) oldest = it;
    if (oldest == lru_.end()) {
      ESP_LOGE(TAG, "Cache cannot free %llu bytes", static_cast<unsigned long long>(needed));
      return false;
    }
    const std::string path = mount_path_ + "/" + oldest->first;
    ESP_LOGI(TAG, "LRU delete: %s", oldest->first.c_str());
    std::remove(path.c_str());
    lru_.erase(oldest);
  }
  save_index_();
  return true;
}

void SoundCache::touch_(const std::string &name) { lru_[name] = ++access_counter_; }

void SoundCache::load_index_() {
  FILE *f = std::fopen((mount_path_ + "/index.tsv").c_str(), "r");
  if (f) {
    char name[96];
    unsigned long long stamp;
    while (std::fscanf(f, "%95s %llu", name, &stamp) == 2) {
      std::string clean;
      if (sanitize_name_(name, clean)) {
        struct stat st{};
        if (stat((mount_path_ + "/" + clean).c_str(), &st) == 0) {
          lru_[clean] = stamp;
          access_counter_ = std::max<uint64_t>(access_counter_, stamp);
        }
      }
    }
    std::fclose(f);
  }
  DIR *dir = opendir(mount_path_.c_str());
  if (!dir) return;
  while (auto *entry = readdir(dir)) {
    std::string clean;
    if (sanitize_name_(entry->d_name, clean) && lru_.find(clean) == lru_.end()) touch_(clean);
    const std::string filename = entry->d_name;
    if (filename.size() > 5 && filename.substr(filename.size() - 5) == ".part")
      std::remove((mount_path_ + "/" + filename).c_str());
  }
  closedir(dir);
  save_index_();
}

void SoundCache::save_index_() {
  const std::string tmp = mount_path_ + "/index.part";
  const std::string dst = mount_path_ + "/index.tsv";
  FILE *f = std::fopen(tmp.c_str(), "w");
  if (!f) return;
  for (const auto &item : lru_) std::fprintf(f, "%s %llu\n", item.first.c_str(), static_cast<unsigned long long>(item.second));
  std::fflush(f);
  std::fclose(f);
  std::remove(dst.c_str());
  std::rename(tmp.c_str(), dst.c_str());
}

void SoundCache::clear() {
  if (busy_) return;
  DIR *dir = opendir(mount_path_.c_str());
  if (dir) {
    while (auto *entry = readdir(dir)) {
      if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) continue;
      std::remove((mount_path_ + "/" + entry->d_name).c_str());
    }
    closedir(dir);
  }
  lru_.clear();
  access_counter_ = 0;
  ESP_LOGI(TAG, "Cache cleared");
}

void SoundCache::stop() {
  cancel_ = true;
  portENTER_CRITICAL(&request_lock_);
  pending_name_.clear();
  portEXIT_CRITICAL(&request_lock_);
  speaker_->stop();
}

}  // namespace esphome::sound_cache
