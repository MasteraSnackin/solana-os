#pragma once

// Decode one baseline JPEG into a caller-owned RGB565 buffer (native endian).
// Uses vendored ChaN TJpgDec (apps/vidplayer/vendor/tjpgd/). Silent / no audio.

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define VIDPLAYER_JPEG_WORK  3100

typedef struct {
    uint16_t width;
    uint16_t height;
    size_t out_bytes;
} vidplayer_jpeg_info_t;

// Fill info (width/height after optional scale) without writing pixels.
// scale: 0=1:1, 1=1:2, 2=1:4, 3=1:8. Returns false on bad/unsupported JPEG.
bool vidplayer_jpeg_info(const uint8_t *jpg, size_t jpg_len, int scale,
                         vidplayer_jpeg_info_t *info);

// Decode into out (RGB565, out_stride pixels per row). out must hold at least
// info.height rows. Centers into a larger buffer if out_w/out_h exceed the
// decoded size and clear_bg is true (fills out with black first).
// Returns false on failure.
bool vidplayer_jpeg_decode(const uint8_t *jpg, size_t jpg_len, int scale,
                           uint16_t *out, int out_w, int out_h, int out_stride,
                           bool clear_bg, vidplayer_jpeg_info_t *info_out);
