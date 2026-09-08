# ESPectre SDK - ESP-IDF component
#
# Registers the shared core and ESP-IDF runtime as a vendored component. Drop
# this tree into your project's `components/` directory and add `espectre` to
# your own component's REQUIRES.
#
# The sensing runtime is always built. The optional capability groups are
# opt-in through menuconfig ("ESPectre SDK") so a minimal integration does not
# pay for transports it never calls. Consumers that include
# `espectre_sources.cmake` directly select the same groups by listing the
# matching ESPECTRE_RUNTIME_ESP_IDF_*_SOURCES variables instead.

include("${CMAKE_CURRENT_LIST_DIR}/espectre_sources.cmake")

set(espectre_sdk_sources
    ${ESPECTRE_CORE_SOURCES}
    ${ESPECTRE_RUNTIME_ESP_IDF_SOURCES}
)

# MiBee vendoring note (2026-09-06): `improv` (git dep, unfetchable here),
# `mqtt` and `esp_https_ota` (moved out of the IDF core in v6.0) are removed
# from REQUIRES below — they serve only the optional capability groups, which
# must stay OFF in menuconfig (see components/espectre/README.md). `mdns` is
# likewise DIRECT-group-only (ai-thinker has no mdns component at all), so it
# is required only when DIRECT is enabled.
set(espectre_sdk_requires
    esp_wifi
    esp_event
    esp_netif
    esp_timer
    # mqtt
    app_update
    esp_http_client
    esp_http_server
    # esp_https_ota
    esp-tls
    nvs_flash
    lwip
    driver
    freertos
    heap
    # improv
    # mdns
    mbedtls
    ${ESPECTRE_SDK_ADDITIONAL_REQUIRES}
)

if(CONFIG_ESPECTRE_SDK_ENABLE_DIRECT)
    list(APPEND espectre_sdk_requires mdns)
endif()

if(ESPECTRE_SDK_EXCLUDED_REQUIRES)
    list(REMOVE_ITEM espectre_sdk_requires ${ESPECTRE_SDK_EXCLUDED_REQUIRES})
endif()

# ESP-IDF expands component requirements before menuconfig values are
# available. Declare the dependencies for every opt-in source group here; the
# source switches below still keep their code out of the linked firmware.

if(CONFIG_ESPECTRE_SDK_ENABLE_FRONTEND_SUPPORT)
    list(APPEND espectre_sdk_sources ${ESPECTRE_RUNTIME_FRONTEND_SUPPORT_SOURCES})
endif()

if(CONFIG_ESPECTRE_SDK_ENABLE_MQTT)
    list(APPEND espectre_sdk_sources ${ESPECTRE_RUNTIME_ESP_IDF_MQTT_SOURCES})
endif()

if(CONFIG_ESPECTRE_SDK_ENABLE_PROVISIONING)
    list(APPEND espectre_sdk_sources ${ESPECTRE_RUNTIME_ESP_IDF_PROVISIONING_SOURCES})
endif()

if(CONFIG_ESPECTRE_SDK_ENABLE_OTA)
    list(APPEND espectre_sdk_sources ${ESPECTRE_RUNTIME_ESP_IDF_OTA_SOURCES})
endif()

if(CONFIG_ESPECTRE_SDK_ENABLE_DIRECT)
    list(APPEND espectre_sdk_sources ${ESPECTRE_RUNTIME_ESP_IDF_DIRECT_SOURCES})
endif()

list(REMOVE_DUPLICATES espectre_sdk_sources)
list(REMOVE_DUPLICATES espectre_sdk_requires)

idf_component_register(
    SRCS
        ${espectre_sdk_sources}
    INCLUDE_DIRS
        "."
        ${ESPECTRE_SHARED_INCLUDE_DIRS}
    REQUIRES
        ${espectre_sdk_requires}
)

idf_component_optional_requires(PRIVATE esp_tinyusb espressif__esp_tinyusb)

# MiBee patch (IDF v6.0-only block): three v6-specific adaptations, skipped
# on IDF 5.5.x (luatos) where the legacy names/APIs still exist —
# 1. mbed TLS 3.6 keeps the legacy SHA-256 API in a private builtin-driver
#    header (mbedtls/private/sha256.h); expose that include root to this
#    component only (device_identity.cpp has a matching __has_include fallback).
# 2. mbed TLS 3.6 hides private-API declarations behind this guard.
# 3. IDF v6.0 renamed the Wi-Fi bandwidth enums (HT prefix dropped):
#    WIFI_BW_HT20/HT40 -> WIFI_BW20/WIFI_BW40 (wifi_lifecycle.cpp).
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    idf_component_get_property(espectre_mbedtls_dir mbedtls COMPONENT_DIR)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${espectre_mbedtls_dir}/mbedtls/tf-psa-crypto/drivers/builtin/include")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
        WIFI_BW_HT20=WIFI_BW20
        WIFI_BW_HT40=WIFI_BW40)
endif()

if(CONFIG_ESPECTRE_TINYUSB_PRIMARY_CONSOLE)
    target_link_options(
        ${COMPONENT_LIB}
        INTERFACE
            "LINKER:--wrap=esp_usb_console_available_for_read"
            "LINKER:--wrap=esp_usb_console_read_buf"
            "LINKER:--wrap=esp_usb_console_write_buf"
    )
endif()

if(CONFIG_ESPECTRE_SDK_ENABLE_DIRECT)
    idf_component_get_property(espectre_mdns_dir espressif__mdns COMPONENT_DIR)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${espectre_mdns_dir}/private_include")
    target_link_options(${COMPONENT_LIB} INTERFACE "LINKER:--wrap=mdns_priv_receive_action")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/espectre_git_version.cmake")
if(ESPECTRE_GIT_VERSION AND NOT ESPECTRE_GIT_VERSION_STAMPED)
    target_compile_definitions(
        ${COMPONENT_LIB}
        PUBLIC
            ESPECTRE_SDK_VERSION_STRING="${ESPECTRE_GIT_VERSION}"
            ESPECTRE_SDK_VERSION_MAJOR=${ESPECTRE_SDK_VERSION_MAJOR}
            ESPECTRE_SDK_VERSION_MINOR=${ESPECTRE_SDK_VERSION_MINOR}
            ESPECTRE_SDK_VERSION_PATCH=${ESPECTRE_SDK_VERSION_PATCH}
    )
endif()
