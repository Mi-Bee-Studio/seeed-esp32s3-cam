# onvif-c — ESP-IDF component
#
# Vendored-component friendly (no idf_component.yml needed): drop this
# tree into components/onvif-c and add "onvif-c" to main's REQUIRES.
#
# NOTE: mdns is intentionally NOT in REQUIRES — since IDF v5.x/6.x it is
# a managed component (espressif/mdns), not a built-in. When the hosting
# project declares it, the header is found and mDNS support compiles in;
# otherwise it compiles out (WS-Discovery still works).

idf_component_register(
    SRCS
        "core/onvif_xml.c"
        "core/onvif_probe.c"
        "core/onvif_events_ring.c"
        "esp_idf/onvif_c_service.c"
        "esp_idf/onvif_c_events.c"
        "esp_idf/onvif_c_discovery.c"
    INCLUDE_DIRS
        "include"
        "core"
        "esp_idf"
    REQUIRES
        esp_http_server
        lwip
        esp_hw_support    # esp_heap_caps (explicit include, PIT lesson)
    PRIV_REQUIRES
        esp_netif
)

# Optional: picked up only when the hosting project provides espressif/mdns
# (managed component since IDF v5.x/6.x); otherwise mDNS compiles out.
idf_component_optional_requires(PRIVATE mdns)

# Host-side golden tests compile core/*.c directly with the system cc —
# no ESP-IDF involved. Run: tests/run.sh
