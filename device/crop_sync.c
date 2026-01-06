#include "device/crop_sync.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct device_crop_sync_s {
pthread_mutex_t mutex;
_Atomic int64_t last_frame_us;
_Atomic uint64_t last_applied_hash;
device_crop_rect_t current_applied;
device_crop_apply_event_t last_event;
device_crop_cmd_t ring[DEVICE_CROP_SYNC_HISTORY];
uint32_t head;
uint32_t count;
uint64_t next_cmd_id;
uint64_t last_option_cmd_id;
} device_crop_sync_t;

typedef struct device_crop_sync_node_s {
struct device_s *dev;
device_crop_sync_t *sync;
struct device_crop_sync_node_s *next;
} device_crop_sync_node_t;

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static device_crop_sync_node_t *g_head = NULL;

static uint64_t crop_hash(bool valid, int32_t x, int32_t y, uint32_t w, uint32_t h) {
uint64_t v = 1469598103934665603ULL;
v ^= (uint64_t)valid; v *= 1099511628211ULL;
v ^= (uint64_t)(uint32_t)x; v *= 1099511628211ULL;
v ^= (uint64_t)(uint32_t)y; v *= 1099511628211ULL;
v ^= (uint64_t)w; v *= 1099511628211ULL;
v ^= (uint64_t)h; v *= 1099511628211ULL;
return v;
}

static bool rect_matches(const device_crop_rect_t *r, bool valid, int32_t x, int32_t y, uint32_t w, uint32_t h) {
return r->valid == valid && r->x == x && r->y == y && r->width == w && r->height == h;
}

static device_crop_sync_t *sync_get(struct device_s *dev, bool create) {
if (!dev) return NULL;
pthread_mutex_lock(&g_mutex);
device_crop_sync_node_t *n = g_head;
while (n) {
if (n->dev == dev) {
device_crop_sync_t *s = n->sync;
pthread_mutex_unlock(&g_mutex);
return s;
}
n = n->next;
}
if (!create) {
pthread_mutex_unlock(&g_mutex);
return NULL;
}
device_crop_sync_t *s = (device_crop_sync_t *)calloc(1, sizeof(device_crop_sync_t));
if (!s) {
pthread_mutex_unlock(&g_mutex);
return NULL;
}
pthread_mutex_init(&s->mutex, NULL);
atomic_store(&s->last_frame_us, 0);
atomic_store(&s->last_applied_hash, 0);
s->current_applied.valid = false;
s->last_event.valid = false;
device_crop_sync_node_t *nn = (device_crop_sync_node_t *)calloc(1, sizeof(device_crop_sync_node_t));
if (!nn) {
pthread_mutex_destroy(&s->mutex);
free(s);
pthread_mutex_unlock(&g_mutex);
return NULL;
}
nn->dev = dev;
nn->sync = s;
nn->next = g_head;
g_head = nn;
pthread_mutex_unlock(&g_mutex);
return s;
}

static void sync_free(device_crop_sync_t *s) {
if (!s) return;
pthread_mutex_destroy(&s->mutex);
free(s);
}

void device_crop_sync_detach(struct device_s *dev) {
if (!dev) return;
pthread_mutex_lock(&g_mutex);
device_crop_sync_node_t *prev = NULL;
device_crop_sync_node_t *n = g_head;
while (n) {
if (n->dev == dev) {
if (prev) prev->next = n->next;
else g_head = n->next;
device_crop_sync_t *s = n->sync;
free(n);
pthread_mutex_unlock(&g_mutex);
sync_free(s);
return;
}
prev = n;
n = n->next;
}
pthread_mutex_unlock(&g_mutex);
}

static device_crop_cmd_t *ring_at(device_crop_sync_t *s, uint32_t idx) {
return &s->ring[idx % DEVICE_CROP_SYNC_HISTORY];
}

static device_crop_cmd_t *ring_newest(device_crop_sync_t *s, uint32_t offset) {
if (offset >= s->count) return NULL;
uint32_t idx = (s->head + DEVICE_CROP_SYNC_HISTORY - 1 - offset) % DEVICE_CROP_SYNC_HISTORY;
return ring_at(s, idx);
}

uint64_t device_crop_sync_submit(struct device_s *dev, bool crop_valid, int32_t x, int32_t y, uint32_t width, uint32_t height) {
device_crop_sync_t *s = sync_get(dev, true);
if (!s) return 0;
pthread_mutex_lock(&s->mutex);
uint64_t cmd_id = ++s->next_cmd_id;
for (uint32_t i = 0; i < s->count; i++) {
device_crop_cmd_t *c = ring_newest(s, i);
if (!c) break;
if (c->first_after_us == 0 && c->state == DEVICE_CROP_CMD_PENDING) {
c->state = DEVICE_CROP_CMD_SUPERSEDED;
c->superseded_by_cmd_id = cmd_id;
break;
}
}
uint32_t insert_idx = s->head;
device_crop_cmd_t *e = ring_at(s, insert_idx);
memset(e, 0, sizeof(*e));
e->cmd_id = cmd_id;
e->state = DEVICE_CROP_CMD_PENDING;
e->superseded_by_cmd_id = 0;
e->submit_time_us = 0;
e->desired.valid = crop_valid;
e->desired.x = x;
e->desired.y = y;
e->desired.width = width;
e->desired.height = height;
e->applied.valid = false;
e->last_before_us = 0;
e->first_after_us = 0;
s->head = (s->head + 1) % DEVICE_CROP_SYNC_HISTORY;
if (s->count < DEVICE_CROP_SYNC_HISTORY) s->count++;
s->last_option_cmd_id = cmd_id;
int64_t lf = atomic_load(&s->last_frame_us);
if (lf != 0 && rect_matches(&s->current_applied, crop_valid, x, y, width, height)) {
e->last_before_us = lf;
e->first_after_us = lf;
e->applied = e->desired;
e->state = DEVICE_CROP_CMD_APPLIED;
s->last_event.valid = true;
s->last_event.cmd_id = cmd_id;
s->last_event.last_before_us = lf;
s->last_event.first_after_us = lf;
s->last_event.applied = e->desired;
}
pthread_mutex_unlock(&s->mutex);
return cmd_id;
}

uint64_t device_crop_sync_take_last_cmd_id(struct device_s *dev) {
device_crop_sync_t *s = sync_get(dev, false);
if (!s) return 0;
pthread_mutex_lock(&s->mutex);
uint64_t id = s->last_option_cmd_id;
s->last_option_cmd_id = 0;
pthread_mutex_unlock(&s->mutex);
return id;
}

void device_crop_sync_on_frame(struct device_s *dev, uint64_t captured_time_us, bool crop_valid, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
device_crop_sync_t *s = sync_get(dev, true);
if (!s) return;
int64_t prev_us = atomic_exchange(&s->last_frame_us, (int64_t)captured_time_us);
int32_t xi = (int32_t)x;
int32_t yi = (int32_t)y;
uint64_t h = crop_hash(crop_valid, xi, yi, width, height);
uint64_t old = atomic_load(&s->last_applied_hash);
if (h == old) return;
pthread_mutex_lock(&s->mutex);
old = atomic_load(&s->last_applied_hash);
if (h != old) {
s->current_applied.valid = crop_valid;
s->current_applied.x = xi;
s->current_applied.y = yi;
s->current_applied.width = width;
s->current_applied.height = height;
atomic_store(&s->last_applied_hash, h);
device_crop_apply_event_t ev;
memset(&ev, 0, sizeof(ev));
ev.valid = true;
ev.cmd_id = 0;
ev.last_before_us = prev_us;
ev.first_after_us = (int64_t)captured_time_us;
ev.applied = s->current_applied;
for (uint32_t i = 0; i < s->count; i++) {
device_crop_cmd_t *c = ring_newest(s, i);
if (!c) break;
if (c->first_after_us != 0) continue;
if (!rect_matches(&c->desired, crop_valid, xi, yi, width, height)) continue;
c->last_before_us = prev_us;
c->first_after_us = (int64_t)captured_time_us;
c->applied = s->current_applied;
if (c->state == DEVICE_CROP_CMD_PENDING) c->state = DEVICE_CROP_CMD_APPLIED;
ev.cmd_id = c->cmd_id;
break;
}
s->last_event = ev;
}
pthread_mutex_unlock(&s->mutex);
}

int device_crop_sync_snapshot(struct device_s *dev, device_crop_sync_snapshot_t *out) {
if (!out) return -1;
device_crop_sync_t *s = sync_get(dev, false);
if (!s) return -1;
memset(out, 0, sizeof(*out));
pthread_mutex_lock(&s->mutex);
out->current_applied = s->current_applied;
out->last_frame_us = atomic_load(&s->last_frame_us);
out->last_event = s->last_event;
out->history_count = (int)s->count;
for (uint32_t i = 0; i < s->count; i++) {
device_crop_cmd_t *c = ring_newest(s, i);
if (!c) break;
out->history[i] = *c;
}
pthread_mutex_unlock(&s->mutex);
return 0;
}

int device_crop_sync_get_cmd(struct device_s *dev, uint64_t cmd_id, device_crop_cmd_t *out) {
if (!out) return -1;
device_crop_sync_t *s = sync_get(dev, false);
if (!s) return -1;
pthread_mutex_lock(&s->mutex);
for (uint32_t i = 0; i < s->count; i++) {
device_crop_cmd_t *c = ring_newest(s, i);
if (!c) break;
if (c->cmd_id == cmd_id) {
*out = *c;
pthread_mutex_unlock(&s->mutex);
return 0;
}
}
pthread_mutex_unlock(&s->mutex);
return -1;
}
