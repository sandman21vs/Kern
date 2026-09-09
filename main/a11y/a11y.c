// The screen reader — see a11y.h
//
// The whole reader hangs off one interception: the touch device's read
// callback is wrapped, so no page in the tree is touched and none of them can
// forget to cooperate. With the reader on, a finger explores instead of
// pressing - the wrapper reports every touch as released and says what is
// under the finger - and a double tap lets the real touch through so the real
// widget reacts.
//
// Wrapping read_cb rather than filtering indev events is deliberate.
// LV_EVENT_PRESSING is never delivered to an indev callback and cannot be
// suppressed, and slider and arc act on exactly that event: a filtered press
// would still drag them. Rewriting the data before LVGL sees it is the only
// place where nothing leaks through.

#include "a11y.h"

#include <stdio.h>
#include <string.h>

#include "../ui/input_helpers.h"
#include "../ui/theme.h"
#include "describe.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "speech.h"

static const char *TAG = "A11Y";

#define MAX_TEXT 128
/* Reading order is capped: past this a screen is not navigable by ear anyway,
 * and the cap keeps the walk bounded on a page that grows unexpectedly. */
#define MAX_STOPS 128

/* Two taps closer together than this, near the same spot, activate. */
#define DOUBLE_TAP_MS 400

/* LVGL needs the synthesised press to survive at least one read cycle before
 * the release, or there is no click to process. */
#define PASS_PRESS_READS 2

static lv_indev_read_cb_t original_read;
static lv_indev_t *touch;
static bool enabled;
static bool installed;

/* Reading order, rebuilt when the screen has changed under us. */
static lv_obj_t *stops[MAX_STOPS];
static size_t stop_count;
static int cursor = -1;

static lv_obj_t *spoken;    /* what the finger is currently on */
static lv_point_t press_at; /* where this touch went down */
static uint64_t press_time;
static uint64_t last_tap_time;
static lv_point_t last_tap_at;
static bool was_pressed;
static bool moved;

/* While non-zero the wrapper is feeding LVGL a real press at pass_point. */
static int pass_reads;
static lv_point_t pass_point;

/* ---------- Finding things ---------- */

static bool contains(lv_obj_t *obj, const lv_point_t *point) {
  lv_area_t area;
  lv_obj_get_coords(obj, &area);
  return point->x >= area.x1 && point->x <= area.x2 && point->y >= area.y1 &&
         point->y <= area.y2;
}

/* Topmost stop under a point.
 *
 * Not lv_indev_search_obj(): that one skips anything without
 * LV_OBJ_FLAG_CLICKABLE, and lv_label_constructor clears that flag on every
 * label. A screen reader that can only find clickable things cannot read text,
 * which is most of what is on a signer's screen. */
static lv_obj_t *stop_at(lv_obj_t *parent, const lv_point_t *point) {
  if (!parent || lv_obj_has_flag(parent, LV_OBJ_FLAG_HIDDEN))
    return NULL;

  /* Last child is topmost, so search backwards and take the first hit. */
  const uint32_t children = lv_obj_get_child_count(parent);
  for (uint32_t i = children; i > 0; i--) {
    lv_obj_t *found = stop_at(lv_obj_get_child(parent, i - 1), point);
    if (found)
      return found;
  }

  if (a11y_is_stop(parent) && contains(parent, point))
    return parent;
  return NULL;
}

static lv_obj_tree_walk_res_t collect(lv_obj_t *obj, void *unused) {
  (void)unused;
  if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN))
    return LV_OBJ_TREE_WALK_SKIP_CHILDREN;
  if (a11y_is_stop(obj)) {
    if (stop_count < MAX_STOPS)
      stops[stop_count++] = obj;
    /* A control reads as one thing; do not descend into its labels. */
    return LV_OBJ_TREE_WALK_SKIP_CHILDREN;
  }
  return LV_OBJ_TREE_WALK_NEXT;
}

/* Rebuild the order in tree order, which for the flex rows Kern lays pages out
 * in is reading order. Cheap enough to do on demand rather than cache and
 * invalidate: a page holds tens of objects, not thousands. */
static void rebuild_order(void) {
  stop_count = 0;
  lv_obj_tree_walk(lv_screen_active(), collect, NULL);
  lv_obj_t *top = lv_display_get_layer_top(NULL);
  if (top)
    lv_obj_tree_walk(top, collect, NULL);
}

static int index_of(lv_obj_t *obj) {
  for (size_t i = 0; i < stop_count; i++)
    if (stops[i] == obj)
      return (int)i;
  return -1;
}

/* ---------- Saying things ---------- */

static void speak(lv_obj_t *obj) {
  char text[MAX_TEXT];
  if (a11y_describe(obj, text, sizeof(text)) == 0)
    return;
  spoken = obj;
  speech_say(text);
}

static void move_cursor(int delta) {
  rebuild_order();
  if (stop_count == 0) {
    speech_earcon(SPEECH_EARCON_BOUNDARY);
    return;
  }
  if (cursor < 0 || cursor >= (int)stop_count)
    cursor = index_of(spoken);
  int next = cursor + delta;
  if (next < 0 || next >= (int)stop_count) {
    speech_earcon(SPEECH_EARCON_BOUNDARY);
    return;
  }
  cursor = next;
  speak(stops[cursor]);
}

static void activate(lv_obj_t *obj) {
  if (!obj)
    return;
  lv_area_t area;
  lv_obj_get_coords(obj, &area);
  pass_point.x = (area.x1 + area.x2) / 2;
  pass_point.y = (area.y1 + area.y2) / 2;
  pass_reads = PASS_PRESS_READS;
  speech_earcon(SPEECH_EARCON_ACTIVATE);
}

/* ---------- The intercept ---------- */

static void read_wrapper(lv_indev_t *indev, lv_indev_data_t *data) {
  /* Always run the real reader. The Espressif touch adapter drains a semaphore
   * in there when the panel is interrupt-driven, so skipping it would wedge
   * the driver, not merely lose a sample. */
  original_read(indev, data);

  if (!enabled)
    return;

  /* Finishing a synthesised activation: hold the press for a couple of reads,
   * then let go. The real widget sees a real press at its own centre. */
  if (pass_reads > 0) {
    data->point = pass_point;
    data->state = LV_INDEV_STATE_PRESSED;
    pass_reads--;
    return;
  }

  const bool pressed = data->state == LV_INDEV_STATE_PRESSED;
  const lv_point_t point = data->point;
  const uint64_t now = esp_timer_get_time() / 1000;

  if (pressed && !was_pressed) {
    press_at = point;
    press_time = now;
    moved = false;
    lv_obj_t *under = stop_at(lv_screen_active(), &point);
    if (under && under != spoken) {
      speech_earcon(SPEECH_EARCON_FOCUS);
      speak(under);
      cursor = -1;
    }
  } else if (pressed) {
    const int32_t dx = point.x - press_at.x;
    const int32_t dy = point.y - press_at.y;
    if (!moved && (LV_ABS(dx) > theme_min_touch_size() / 6 ||
                   LV_ABS(dy) > theme_min_touch_size() / 6))
      moved = true;
    if (moved) {
      /* Exploring: whatever is under the finger, say it once. */
      lv_obj_t *under = stop_at(lv_screen_active(), &point);
      if (under && under != spoken) {
        speech_earcon(SPEECH_EARCON_FOCUS);
        speak(under);
        cursor = -1;
      }
    }
  } else if (!pressed && was_pressed) {
    const int32_t dx = point.x - press_at.x;
    const int32_t dy = point.y - press_at.y;
    lv_dir_t dir;
    if (moved && ui_drag_is_swipe(dx, dy, &dir)) {
      if (dir == LV_DIR_RIGHT)
        move_cursor(1);
      else if (dir == LV_DIR_LEFT)
        move_cursor(-1);
    } else if (!moved) {
      const bool near =
          LV_ABS(press_at.x - last_tap_at.x) < theme_min_touch_size() &&
          LV_ABS(press_at.y - last_tap_at.y) < theme_min_touch_size();
      if (near && now - last_tap_time < DOUBLE_TAP_MS) {
        activate(spoken);
        last_tap_time = 0;
      } else {
        last_tap_time = now;
        last_tap_at = press_at;
      }
    }
  }

  was_pressed = pressed;

  /* Nothing the finger did reaches the widgets. The one exception is the
   * synthesised press above, which returns before getting here. */
  data->state = LV_INDEV_STATE_RELEASED;
}

/* ---------- Public API ---------- */

bool a11y_init(void) {
  if (installed)
    return speech_available();

  if (!speech_init()) {
    ESP_LOGW(TAG, "No voice; screen reader unavailable");
    return false;
  }

  for (lv_indev_t *indev = lv_indev_get_next(NULL); indev;
       indev = lv_indev_get_next(indev)) {
    if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER)
      continue;
    original_read = lv_indev_get_read_cb(indev);
    lv_indev_set_read_cb(indev, read_wrapper);
    touch = indev;
    break;
  }
  if (!touch) {
    ESP_LOGW(TAG, "No pointer input to wrap");
    speech_deinit();
    return false;
  }

  installed = true;
  ESP_LOGI(TAG, "Screen reader installed");
  return true;
}

void a11y_set_enabled(bool on) {
  if (enabled == on)
    return;
  enabled = on;
  spoken = NULL;
  cursor = -1;
  was_pressed = false;
  pass_reads = 0;
  if (!on)
    speech_silence();
}

bool a11y_enabled(void) { return enabled && installed; }

void a11y_announce(const char *text) {
  if (!a11y_enabled() || !text)
    return;
  spoken = NULL; /* the screen changed under the finger */
  cursor = -1;
  speech_say(text);
}

void a11y_silence(void) {
  if (installed)
    speech_silence();
}
