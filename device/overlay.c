#include "device/overlay.h"
#include "device/buffer_list.h"

#include <linux/videodev2.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

static const uint8_t font3x5[][5] = {
  { 0x07, 0x05, 0x05, 0x05, 0x07 }, // 0
  { 0x02, 0x06, 0x02, 0x02, 0x07 }, // 1
  { 0x07, 0x01, 0x07, 0x04, 0x07 }, // 2
  { 0x07, 0x01, 0x07, 0x01, 0x07 }, // 3
  { 0x05, 0x05, 0x07, 0x01, 0x01 }, // 4
  { 0x07, 0x04, 0x07, 0x01, 0x07 }, // 5
  { 0x07, 0x04, 0x07, 0x05, 0x07 }, // 6
  { 0x07, 0x01, 0x01, 0x01, 0x01 }, // 7
  { 0x07, 0x05, 0x07, 0x05, 0x07 }, // 8
  { 0x07, 0x05, 0x07, 0x01, 0x07 }, // 9
};

static void yuyv_set_y(uint8_t *base, unsigned width, unsigned x, unsigned y, uint8_t yval)
{
  size_t off = ((size_t)y * width + x) * 2;
  base[off] = yval;
}

static void draw_char_3x5(uint8_t *base, unsigned width, unsigned height, unsigned x0, unsigned y0, char c, unsigned scale, uint8_t yval)
{
  if (c < '0' || c > '9')
    return;

  const uint8_t *glyph = font3x5[c - '0'];

  for (unsigned ry = 0; ry < 5; ry++) {
    uint8_t row = glyph[ry];
    for (unsigned rx = 0; rx < 3; rx++) {
      if (row & (1u << (2 - rx))) {
        for (unsigned sy = 0; sy < scale; sy++) {
          unsigned yy = y0 + ry * scale + sy;
          if (yy >= height)
            continue;
          for (unsigned sx = 0; sx < scale; sx++) {
            unsigned xx = x0 + rx * scale + sx;
            if (xx >= width)
              continue;
            yuyv_set_y(base, width, xx, yy, yval);
          }
        }
      }
    }
  }
}

static void draw_comma(uint8_t *base, unsigned width, unsigned height, unsigned x0, unsigned y0, unsigned scale, uint8_t yval)
{
  unsigned xx = x0 + 1 * scale;
  unsigned yy = y0 + 4 * scale;
  for (unsigned sy = 0; sy < scale; sy++) {
    for (unsigned sx = 0; sx < scale; sx++) {
      unsigned x = xx + sx;
      unsigned y = yy + sy;
      if (x < width && y < height)
        yuyv_set_y(base, width, x, y, yval);
    }
  }
}

static unsigned draw_text_line(uint8_t *base, unsigned width, unsigned height, unsigned x, unsigned y, const char *text, unsigned scale, uint8_t y_white)
{
  for (size_t i = 0; i < strlen(text); i++) {
    char c = text[i];

    if (c == ',') {
      if (x + 2 * scale >= width)
        break;
      draw_comma(base, width, height, x, y, scale, y_white);
      x += 2 * scale;
      continue;
    }

    if (c >= '0' && c <= '9') {
      if (x + 4 * scale >= width)
        break;
      draw_char_3x5(base, width, height, x, y, c, scale, y_white);
      x += 4 * scale;
      continue;
    }
  }
  return x;
}

void overlay_draw_crop_yuyv(buffer_t *buf)
{
  if (!buf || !buf->start || !buf->buf_list)
    return;

  if (buf->buf_list->fmt.format != V4L2_PIX_FMT_YUYV)
    return;

  unsigned width = buf->buf_list->fmt.width;
  unsigned height = buf->buf_list->fmt.height;
  uint8_t *base = (uint8_t *)buf->start;

  char crop_text[128];
  if (buf->crop_valid) {
    snprintf(crop_text, sizeof(crop_text), "%u,%u,%u,%u",
      buf->crop_x, buf->crop_y, buf->crop_width, buf->crop_height);
  } else {
    snprintf(crop_text, sizeof(crop_text), "0,0,0,0");
  }

  char ts_text[128];
  snprintf(ts_text, sizeof(ts_text), "%" PRIu64, buf->captured_time_us);

  unsigned margin = 8;

  unsigned scale_crop = 2;
  unsigned scale_ts = 1;

  unsigned line_h_crop = 5 * scale_crop;
  unsigned line_h_ts = 5 * scale_ts;

  unsigned gap = 4;

  unsigned x0 = margin;
  unsigned y0 = margin;

  uint8_t y_white = 235;

  draw_text_line(base, width, height, x0, y0, crop_text, scale_crop, y_white);

  unsigned y1 = y0 + line_h_crop + gap;
  if (y1 + line_h_ts < height) {
    draw_text_line(base, width, height, x0, y1, ts_text, scale_ts, y_white);
  }
}
