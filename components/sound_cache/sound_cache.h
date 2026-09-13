#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <string>

#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome::sound_cache {

class SoundCache : public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_speaker(speaker::Speaker *speaker) { speaker_ = speaker; }
  void set_base_url(const std::string &base_url) { base_url_ = base_url; }
  void set_partition_label(const std::string &label) { partition_label_ = label; }
  void set_reserve_bytes(uint32_t bytes) { reserve_bytes_ = bytes; }
  void set_max_file_size(uint32_t bytes) { max_file_size_ = bytes; }

  void play(const std::string &name, float volume);
  void stop();
  void clear();

 protected:
  static void task_entry_(void *param);
  void task_();
  bool sanitize_name_(const std::string &input, std::string &name) const;
  bool ensure_cached_(const std::string &name, std::string &path);
  bool download_(const std::string &name, const std::string &tmp_path);
  bool validate_wav_(const std::string &path, uint32_t &data_offset, uint32_t &data_size);
  bool play_file_(const std::string &path, float volume);
  bool ensure_space_(uint64_t needed, const std::string &protected_name);
  uint64_t free_bytes_() const;
  void load_index_();
  void save_index_();
  void touch_(const std::string &name);

  speaker::Speaker *speaker_{nullptr};
  std::string base_url_;
  std::string partition_label_{"soundcache"};
  std::string mount_path_{"/soundcache"};
  uint32_t reserve_bytes_{262144};
  uint32_t max_file_size_{4194304};
  std::map<std::string, uint64_t> lru_;
  uint64_t access_counter_{0};
  std::string pending_name_;
  float pending_volume_{0.60f};
  std::atomic<bool> busy_{false};
  std::atomic<bool> cancel_{false};
  portMUX_TYPE request_lock_ = portMUX_INITIALIZER_UNLOCKED;
};

}  // namespace esphome::sound_cache
