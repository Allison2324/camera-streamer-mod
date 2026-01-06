#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_CROP_SYNC_HISTORY 64

typedef enum device_crop_cmd_state_e {
DEVICE_CROP_CMD_PENDING = 0,
DEVICE_CROP_CMD_APPLIED = 1,
DEVICE_CROP_CMD_SUPERSEDED = 2
} device_crop_cmd_state_t;

typedef struct device_crop_rect_s {
bool valid;
int32_t x;
int32_t y;
uint32_t width;
uint32_t height;
} device_crop_rect_t;

typedef struct device_crop_cmd_s {
uint64_t cmd_id;
device_crop_cmd_state_t state;
uint64_t superseded_by_cmd_id;
int64_t submit_time_us;
device_crop_rect_t desired;
int64_t last_before_us;
int64_t first_after_us;
device_crop_rect_t applied;
} device_crop_cmd_t;

typedef struct device_crop_apply_event_s {
bool valid;
uint64_t cmd_id;
int64_t last_before_us;
int64_t first_after_us;
device_crop_rect_t applied;
} device_crop_apply_event_t;

typedef struct device_crop_sync_snapshot_s {
device_crop_rect_t current_applied;
int64_t last_frame_us;
device_crop_apply_event_t last_event;
device_crop_cmd_t history[DEVICE_CROP_SYNC_HISTORY];
int history_count;
} device_crop_sync_snapshot_t;

struct device_s;

uint64_t device_crop_sync_submit(struct device_s *dev, bool crop_valid, int32_t x, int32_t y, uint32_t width, uint32_t height);
void device_crop_sync_on_frame(struct device_s *dev, uint64_t captured_time_us, bool crop_valid, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
uint64_t device_crop_sync_take_last_cmd_id(struct device_s *dev);
int device_crop_sync_snapshot(struct device_s *dev, device_crop_sync_snapshot_t *out);
int device_crop_sync_get_cmd(struct device_s *dev, uint64_t cmd_id, device_crop_cmd_t *out);
void device_crop_sync_detach(struct device_s *dev);

#ifdef __cplusplus
}
#endif
