// Widget -> words (see describe.h)
//
// Kern has no accessibility annotations on its widgets and this deliberately
// does not add any: 22 menus and 40-odd pages would each need touching, and a
// label that goes stale is worse than one derived from what is on screen. So
// the description is read out of the widget tree itself, which is also why it
// stays correct when a page changes.

#include "describe.h"

#include <string.h>

#include "../ui/assets/icons.h"

/* Font Awesome glyphs and LVGL's own symbols live in the private-use area and
 * arrive here as multi-byte UTF-8. There is nothing to say for a picture, and
 * a label holding only one is not a stop. */
static bool has_speakable(const char *text) {
  for (const char *c = text; *c; c++)
    if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
        (*c >= '0' && *c <= '9'))
      return true;
  return false;
}

static size_t append(char *out, size_t max, size_t at, const char *text) {
  if (at && at + 1 < max)
    out[at++] = ' ';
  const size_t len = strlen(text);
  const size_t room = (at < max) ? max - at - 1 : 0;
  const size_t take = len < room ? len : room;
  memcpy(out + at, text, take);
  out[at + take] = '\0';
  return at + take;
}

/* Depth-first over the labels inside a control. A menu entry is an icon label
 * and a text label in a container; a dialog button is one label. */
static size_t collect_labels(lv_obj_t *obj, char *out, size_t max, size_t at) {
  if (lv_obj_check_type(obj, &lv_label_class)) {
    const char *text = lv_label_get_text(obj);
    if (text && has_speakable(text))
      return append(out, max, at, text);
    return at;
  }
  const uint32_t children = lv_obj_get_child_count(obj);
  for (uint32_t i = 0; i < children; i++) {
    lv_obj_t *child = lv_obj_get_child(obj, i);
    if (child && !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN))
      at = collect_labels(child, out, max, at);
  }
  return at;
}

/* Names for controls that carry no text of their own. Small and fixed: only
 * the four corner buttons use it, and a screen holds at most a couple. Each
 * entry clears itself when its object is deleted, so a recycled address can
 * never inherit someone else's name. */
#define MAX_NAMES 16

static struct {
  lv_obj_t *obj;
  const char *name;
} names[MAX_NAMES];

static void forget_name(lv_event_t *e) {
  lv_obj_t *obj = lv_event_get_target(e);
  for (size_t i = 0; i < MAX_NAMES; i++)
    if (names[i].obj == obj) {
      names[i].obj = NULL;
      names[i].name = NULL;
      return;
    }
}

void a11y_label(lv_obj_t *obj, const char *name) {
  if (!obj || !name)
    return;
  for (size_t i = 0; i < MAX_NAMES; i++) {
    if (names[i].obj == obj) {
      names[i].name = name;
      return;
    }
  }
  for (size_t i = 0; i < MAX_NAMES; i++) {
    if (!names[i].obj) {
      names[i].obj = obj;
      names[i].name = name;
      lv_obj_add_event_cb(obj, forget_name, LV_EVENT_DELETE, NULL);
      return;
    }
  }
  /* Full. Dropping the name costs a silent control, which is better than
   * evicting one that is still on screen. */
}

static const char *name_of(lv_obj_t *obj) {
  for (size_t i = 0; i < MAX_NAMES; i++)
    if (names[i].obj == obj)
      return names[i].name;
  return NULL;
}

bool a11y_is_stop(lv_obj_t *obj) {
  if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN))
    return false;

  /* A label carries its own text and never contains a control. */
  if (lv_obj_check_type(obj, &lv_label_class)) {
    const char *text = lv_label_get_text(obj);
    return text && has_speakable(text);
  }

  /* Anything the user can operate is a stop, and the reader does not look
   * inside it: a menu entry should read as one thing, not as an icon and then
   * a word. */
  if (name_of(obj))
    return true;
  return lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) ||
         lv_obj_check_type(obj, &lv_switch_class) ||
         lv_obj_check_type(obj, &lv_dropdown_class) ||
         lv_obj_check_type(obj, &lv_textarea_class);
}

size_t a11y_describe(lv_obj_t *obj, char *out, size_t max) {
  if (!obj || !out || max < 2)
    return 0;
  out[0] = '\0';

  const char *given = name_of(obj);
  size_t at =
      given ? append(out, max, 0, given) : collect_labels(obj, out, max, 0);

  if (lv_obj_check_type(obj, &lv_switch_class))
    at = append(out, max, at,
                lv_obj_has_state(obj, LV_STATE_CHECKED) ? "on" : "off");
  else if (lv_obj_check_type(obj, &lv_dropdown_class)) {
    char selected[64];
    lv_dropdown_get_selected_str(obj, selected, sizeof(selected));
    if (has_speakable(selected))
      at = append(out, max, at, selected);
  } else if (lv_obj_check_type(obj, &lv_textarea_class)) {
    const char *text = lv_textarea_get_text(obj);
    if (text && has_speakable(text))
      at = append(out, max, at, text);
    else
      at = append(out, max, at, "empty");
  }

  if (lv_obj_has_state(obj, LV_STATE_DISABLED))
    at = append(out, max, at, "dimmed");

  return at;
}
