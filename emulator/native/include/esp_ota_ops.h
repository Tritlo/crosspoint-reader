#pragma once

#include <cstddef>

struct esp_partition_t {
  size_t size;
};

inline const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*) {
  static constexpr esp_partition_t unavailable{0};
  return &unavailable;
}
