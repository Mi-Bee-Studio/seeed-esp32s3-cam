# wm_jpge — Minimal baseline JPEG encoder, forked from
# https://github.com/richgel999/jpeg-compressor (master 2024).
# Dual-licensed public domain / Apache-2.0 (GPL-3.0 compatible).
# FORK NOTE: namespace renamed jpge -> wm_jpge. esp32-camera vendors its own
# jpge internally (conversions/jpge.cpp) that is NOT header-exposed — without
# this rename both copies satisfy jpeg_encoder symbols (ODR lottery).
# No other functional change.
idf_component_register(SRCS "jpge.cpp"
                       INCLUDE_DIRS ".")
