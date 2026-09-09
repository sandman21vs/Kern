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

/* Spacing of the reads the activation timer drives. Long enough for LVGL to
 * process each one as a distinct step, short enough to feel immediate. */
#define PASS_TICK_MS 20

/* Steps of the synthesised touch, one per tick: hold, hold, let go, done. The
 * press is held for two so LVGL sees a stable press before the release. */
#define PASS_STEPS 4

static lv_indev_read_cb_t original_read;
static lv_indev_t *touch;
static bool enabled;
static bool installed;
static bool passthrough;
static bool speak_secrets;

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

/* The synthesised touch that activation drives. Phase rather than a count of
 * reads: lv_indev_read() runs the read callback more than once per call - it
 * loops while the data says to keep reading - so anything that decremented per
 * callback would race through the press and never deliver it. */
typedef enum {
  PASS_IDLE,
  PASS_PRESSING,
  PASS_RELEASING,
} pass_phase_t;

static pass_phase_t pass_phase;
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
  /* Masked material never reaches the speaker unless it was explicitly asked
   * for. The reader still stops here and still says something, because going
   * silent would read as a dead area of the screen rather than a protected
   * one. */
  if (!speak_secrets && a11y_is_masked(obj)) {
    spoken = obj;
    speech_earcon(SPEECH_EARCON_MASKED);
    return;
  }

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

/* Drives the synthesised press itself rather than waiting for the input device
 * to be read again.
 *
 * A polled input device would deliver those reads on its own, and on the
 * device it does - the Espressif touch adapter runs the indev in timer mode.
 * The simulator's SDL mouse runs it in LV_INDEV_MODE_EVENT, where a read only
 * happens when SDL reports an event: after the second tap's button-up there
 * are no more events, so the press would sit undelivered until the user moved
 * the mouse, and then fire late and somewhere else. Pumping the reads here
 * makes activation behave the same under both modes. */
static void pass_tick(lv_timer_t *timer) {
  int *step = lv_timer_get_user_data(timer);

  switch (++(*step)) {
  case 1:
  case 2:
    pass_phase = PASS_PRESSING;
    break;
  case 3:
    pass_phase = PASS_RELEASING;
    break;
  default:
    pass_phase = PASS_IDLE;
    lv_free(step);
    lv_timer_delete(timer);
    return;
  }
  lv_indev_read(touch);
}

static void activate(lv_obj_t *obj) {
  if (!obj || pass_phase != PASS_IDLE)
    return;

  lv_area_t area;
  lv_obj_get_coords(obj, &area);
  pass_point.x = (area.x1 + area.x2) / 2;
  pass_point.y = (area.y1 + area.y2) / 2;

  int *step = lv_malloc(sizeof(int));
  if (!step)
    return;
  *step = 0;

  speech_earcon(SPEECH_EARCON_ACTIVATE);

  /* Not driven inline: this runs from inside the read callback, and
   * lv_indev_read() from there would re-enter it. */
  lv_timer_t *timer = lv_timer_create(pass_tick, PASS_TICK_MS, step);
  if (!timer)
    lv_free(step);
}

/* ---------- The intercept ---------- */

static void read_wrapper(lv_indev_t *indev, lv_indev_data_t *data) {
  /* Always run the real reader. The Espressif touch adapter drains a semaphore
   * in there when the panel is interrupt-driven, so skipping it would wedge
   * the driver, not merely lose a sample. */
  original_read(indev, data);

  if (!enabled)
    return;

  /* An overlay that is dismissed by any press is up. Suppressing that press is
   * how a reader user gets stuck on it, so stand aside entirely. */
  if (passthrough)
    return;

  /* Mid-activation: report the synthesised touch at the announced widget's
   * centre and nothing else. The phase is advanced by the timer, not here. */
  if (pass_phase != PASS_IDLE) {
    data->point = pass_point;
    data->state = (pass_phase == PASS_PRESSING) ? LV_INDEV_STATE_PRESSED
                                                : LV_INDEV_STATE_RELEASED;
    return;
  }

  const bool pressed = data->state == LV_INDEV_STATE_PRESSED;
  const lv_point_t point = data->point;
  const uint64_t now = esp_timer_get_time() / 1000;

  /* Every touch is reported to LVGL as released, and LVGL only counts a press
   * as activity - so without this a finger exploring the screen looks exactly
   * like a device nobody has touched. The screensaver comes up under the
   * user's hand, the session lock follows, and neither can be dismissed
   * because the touches that would dismiss them are the ones being
   * suppressed. Report the activity even though the press is swallowed. */
  if (pressed)
    lv_display_trigger_activity(lv_indev_get_display(indev));

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
  pass_phase = PASS_IDLE;
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

void a11y_set_passthrough(bool on) {
  passthrough = on;
  if (on) {
    /* Whatever was being described is about to be covered up. */
    spoken = NULL;
    cursor = -1;
    pass_phase = PASS_IDLE;
  }
}

void a11y_set_speak_secrets(bool allowed) { speak_secrets = allowed; }

bool a11y_speak_secrets(void) { return speak_secrets; }

void a11y_silence(void) {
  if (installed)
    speech_silence();
}
