// Thin TJpgDec wrapper → RGB565 for the silent MJPEG player.

#include "vidplayer_jpeg.h"

#include <string.h>

#include "esp_log.h"

#include "apps/vidplayer/vendor/tjpgd/tjpgd.h"

static const char *TAG = "vidjpeg";

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    uint16_t *out;
    int out_w;
    int out_h;
    int out_stride;
    int ox;
    int oy;
    int scale;
} jpeg_io_t;

static size_t in_cb(JDEC *jd, uint8_t *buf, size_t nbyte)
{
    jpeg_io_t *io = (jpeg_io_t *)jd->device;
    if (io->pos >= io->len) {
        return 0;
    }
    if (io->pos + nbyte > io->len) {
        nbyte = io->len - io->pos;
    }
    if (buf) {
        memcpy(buf, io->data + io->pos, nbyte);
    }
    io->pos += nbyte;
    return nbyte;
}

static int out_cb(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_io_t *io = (jpeg_io_t *)jd->device;
    const uint16_t *src = (const uint16_t *)bitmap;
    (void)jd;
    for (int y = (int)rect->top; y <= (int)rect->bottom; y++) {
        int dy = io->oy + y;
        if (dy < 0 || dy >= io->out_h) {
            src += (rect->right - rect->left + 1);
            continue;
        }
        uint16_t *dst = io->out + (size_t)dy * (size_t)io->out_stride;
        for (int x = (int)rect->left; x <= (int)rect->right; x++) {
            int dx = io->ox + x;
            uint16_t c = *src++;
            if (dx >= 0 && dx < io->out_w) {
                dst[dx] = c;
            }
        }
    }
    return 1;
}

bool vidplayer_jpeg_info(const uint8_t *jpg, size_t jpg_len, int scale,
                         vidplayer_jpeg_info_t *info)
{
    if (!jpg || jpg_len < 4 || !info || scale < 0 || scale > 3) {
        return false;
    }
    uint8_t work[VIDPLAYER_JPEG_WORK];
    jpeg_io_t io = { .data = jpg, .len = jpg_len, .pos = 0, .scale = scale };
    JDEC jd;
    JRESULT r = jd_prepare(&jd, in_cb, work, sizeof(work), &io);
    if (r != JDR_OK) {
        ESP_LOGD(TAG, "prepare %d", (int)r);
        return false;
    }
    int div = 1 << scale;
    info->width = (uint16_t)(jd.width / div);
    info->height = (uint16_t)(jd.height / div);
    info->out_bytes = (size_t)info->width * (size_t)info->height * 2;
    return true;
}

bool vidplayer_jpeg_decode(const uint8_t *jpg, size_t jpg_len, int scale,
                           uint16_t *out, int out_w, int out_h, int out_stride,
                           bool clear_bg, vidplayer_jpeg_info_t *info_out)
{
    if (!jpg || !out || jpg_len < 4 || out_w <= 0 || out_h <= 0 || out_stride < out_w ||
        scale < 0 || scale > 3) {
        return false;
    }
    uint8_t work[VIDPLAYER_JPEG_WORK];
    jpeg_io_t io = {
        .data = jpg,
        .len = jpg_len,
        .pos = 0,
        .out = out,
        .out_w = out_w,
        .out_h = out_h,
        .out_stride = out_stride,
        .scale = scale,
    };
    JDEC jd;
    JRESULT r = jd_prepare(&jd, in_cb, work, sizeof(work), &io);
    if (r != JDR_OK) {
        ESP_LOGW(TAG, "prepare failed %d", (int)r);
        return false;
    }
    int div = 1 << scale;
    int dw = (int)(jd.width / div);
    int dh = (int)(jd.height / div);
    io.ox = (out_w - dw) / 2;
    io.oy = (out_h - dh) / 2;
    if (clear_bg) {
        for (int y = 0; y < out_h; y++) {
            memset(out + (size_t)y * (size_t)out_stride, 0, (size_t)out_w * 2);
        }
    }
    r = jd_decomp(&jd, out_cb, (uint8_t)scale);
    if (r != JDR_OK) {
        ESP_LOGW(TAG, "decomp failed %d", (int)r);
        return false;
    }
    if (info_out) {
        info_out->width = (uint16_t)dw;
        info_out->height = (uint16_t)dh;
        info_out->out_bytes = (size_t)dw * (size_t)dh * 2;
    }
    return true;
}
