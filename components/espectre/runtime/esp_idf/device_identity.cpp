/*
 * ESPectre - Device Identity
 *
 * Derives stable runtime device identifiers from platform identity data.
 *
 * Author: Francesco Pace <francesco.pace@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-only
 * Commercial licensing available under separate agreement; see LICENSING.md.
 */
#include "device_identity.h"

#include "espectre_protocol.h"

#include <array>
#include <cstring>

#include <esp_mac.h>
// MiBee patch (IDF v6.0): mbed TLS 3.6 moved the legacy header to
// mbedtls/private/. Upstream builds on IDF 5.5 where the old path exists.
#if __has_include(<mbedtls/sha256.h>)
#include <mbedtls/sha256.h>
#else
#include <mbedtls/private/sha256.h>
#endif

namespace espectre {

namespace {

constexpr char kDeviceIdDomain[] = "espectre-device-id-v1";

uint64_t derive_device_id_once() {
  uint8_t mac[6] = {0U, 0U, 0U, 0U, 0U, 0U};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    return ESPECTRE_DEFAULT_DEVICE_ID;
  }

  std::array<unsigned char, sizeof(kDeviceIdDomain) - 1U + sizeof(mac)> input{};
  std::memcpy(input.data(), kDeviceIdDomain, sizeof(kDeviceIdDomain) - 1U);
  std::memcpy(input.data() + sizeof(kDeviceIdDomain) - 1U, mac, sizeof(mac));

  std::array<unsigned char, 32U> digest{};
  // MiBee patch (IDF v6.0): one-shot mbedtls_sha256() is gone from the
  // private header — use the context API, which exists in every mbed TLS.
  mbedtls_sha256_context sha_ctx;
  mbedtls_sha256_init(&sha_ctx);
  bool ok = mbedtls_sha256_starts(&sha_ctx, 0) == 0 &&
            mbedtls_sha256_update(&sha_ctx, input.data(), input.size()) == 0 &&
            mbedtls_sha256_finish(&sha_ctx, digest.data()) == 0;
  mbedtls_sha256_free(&sha_ctx);
  if (!ok) {
    return ESPECTRE_DEFAULT_DEVICE_ID;
  }

  uint64_t device_id = 0U;
  for (size_t i = 0U; i < sizeof(device_id); ++i) {
    device_id = (device_id << 8U) | static_cast<uint64_t>(digest[i]);
  }
  return device_id;
}

}  // namespace

uint64_t derive_runtime_device_id() {
  static const uint64_t device_id = derive_device_id_once();
  return device_id;
}

std::string derive_runtime_device_id_string() {
  static const std::string device_id = format_espectre_device_id(derive_runtime_device_id());
  return device_id;
}

}  // namespace espectre
