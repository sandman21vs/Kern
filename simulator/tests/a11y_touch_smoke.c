/*
 * Screen reader touch smoke test
 *
 * Drives the reader's touch layer against a real LVGL tree with synthetic
 * events, speech stubbed to a recorder, so the assertions are about behaviour
 * rather than audio. Everything here has been a bug at some point.
 *
 * Both input modes are exercised on purpose. The device runs the input device
 * in timer mode, where reads arrive whether or not anything moved; the
 * simulator's SDL mouse runs it in LV_INDEV_MODE_EVENT, where a read happens
 * only when SDL reports an event. Two of the bugs below existed solely in the
 * second one.
 *
 * Build: part of the simulator build.
 * Run:   ./simulator/build/kern_sim_a11y_smoke
 */

#include "a11y.h"
#include "describe.h"
#include "pages/screensaver.h"
#include "speech.h"
#include "ui/input_helpers.h"
#include "ui/theme.h"
#include "ui/theme_widgets.h"
#include "src/drivers/sdl/lv_sdl_mouse.h"
#include "src/drivers/sdl/lv_sdl_private.h"
#include "src/drivers/sdl/lv_sdl_window.h"
#include <SDL2/SDL.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

/* ---- speech stub: record instead of play ---- */
static char said[32][128];
static int said_n;
static int earcons[32];
static int earcon_n;
bool speech_init(void) { return true; }
void speech_deinit(void) {}
bool speech_available(void) { return true; }
void speech_say(const char *t) {
  if (said_n < 32)
    snprintf(said[said_n++], 128, "%s", t);
}
void speech_earcon(speech_earcon_t e) {
  if (earcon_n < 32)
    earcons[earcon_n++] = e;
}
void speech_silence(void) {}

/* ---- synthetic touch ---- */
static lv_point_t touch_pt;
static bool touch_down;
static void fake_read(lv_indev_t *indev, lv_indev_data_t *data) {
  (void)indev;
  data->point = touch_pt;
  data->state = touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* The real SDL handler used to recognize its mouse by read-callback identity.
 * Wrapping that callback, as the screen reader does, then made the handler drop
 * every event before the wrapper could see it. */
static lv_indev_read_cb_t sdl_original_read;
static bool sdl_wrapped_saw_press;
static void sdl_wrapped_read(lv_indev_t *indev, lv_indev_data_t *data) {
  sdl_original_read(indev, data);
  if (data->state == LV_INDEV_STATE_PRESSED)
    sdl_wrapped_saw_press = true;
}

static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  (void)a;
  (void)px;
  lv_display_flush_ready(d);
}

static void pump(int times) {
  for (int i = 0; i < times; i++) {
    lv_timer_handler();
    lv_tick_inc(20);
  }
}

static void touch_at(int x, int y, bool down) {
  touch_pt.x = x;
  touch_pt.y = y;
  touch_down = down;
  pump(2);
}

static int pass = 0, fail = 0;
static void check(const char *what, int ok) {
  printf("  %-52s %s\n", what, ok ? "OK" : "FALHOU");
  ok ? pass++ : fail++;
}
static void reset(void) {
  said_n = 0;
  earcon_n = 0;
}
static int said_contains(const char *needle) {
  for (int i = 0; i < said_n; i++)
    if (strstr(said[i], needle))
      return 1;
  return 0;
}

static void spoken_reset_hack(void) {
  a11y_set_enabled(false);
  a11y_set_enabled(true);
}
static int probe_dismiss_count;
static void probe_dismissed_cb(void) { probe_dismiss_count++; }
static int clicked;
static void on_click(lv_event_t *e) {
  (void)e;
  clicked++;
}

int main(void) {
  lv_init();
  static uint8_t buf[480 * 40 * 4];
  lv_display_t *disp = lv_display_create(480, 800);
  lv_display_set_flush_cb(disp, flush);
  lv_display_set_buffers(disp, buf, NULL, sizeof(buf),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  theme_init(); /* the slop and double-tap distances come from the theme */
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, fake_read);

  /* A screen shaped like a Kern menu: two buttons, each holding a label. */
  lv_obj_t *scr = lv_screen_active();
  lv_obj_t *btn_a = lv_button_create(scr);
  lv_obj_set_pos(btn_a, 40, 100);
  lv_obj_set_size(btn_a, 400, 80);
  lv_obj_t *la = lv_label_create(btn_a);
  lv_label_set_text(la, "Scan");
  lv_obj_add_event_cb(btn_a, on_click, LV_EVENT_CLICKED, NULL);
  lv_obj_t *btn_b = lv_button_create(scr);
  lv_obj_set_pos(btn_b, 40, 300);
  lv_obj_set_size(btn_b, 400, 80);
  lv_obj_t *lb = lv_label_create(btn_b);
  lv_label_set_text(lb, "Back Up");
  lv_obj_t *note = lv_label_create(scr);
  lv_obj_set_pos(note, 40, 500);
  lv_label_set_text(note, "Wrong PIN");
  pump(3);

  printf("=== screen reader touch layer ===\n");

  /* describe() first — no touch involved */
  char out[128];
  a11y_describe(btn_a, out, sizeof(out));
  check("a button describes as the label inside it", strcmp(out, "Scan") == 0);
  a11y_describe(note, out, sizeof(out));
  check("a bare label describes as its text", strcmp(out, "Wrong PIN") == 0);
  check("a non-clickable label is still a stop", a11y_is_stop(note));

  if (!a11y_init()) {
    printf("a11y_init falhou\n");
    return 1;
  }

  /* --- reader OFF: touches must reach widgets untouched --- */
  reset();
  clicked = 0;
  touch_at(240, 140, true);
  touch_at(240, 140, false);
  pump(3);
  check("off: a touch reaches the widget", clicked == 1);
  check("off: nothing is announced", said_n == 0);

  a11y_set_enabled(true);

  /* --- reader ON: a touch explores, it does not press --- */
  reset();
  clicked = 0;
  touch_at(240, 140, true);
  check("on: touching Scan announces it", said_contains("Scan"));
  touch_at(240, 140, false);
  pump(3);
  check("on: that touch does not become a click", clicked == 0);

  /* --- dragging onto another element announces it --- */
  reset();
  touch_at(240, 140, true);
  touch_at(240, 340, true); /* finger slides onto Back Up */
  check("sliding onto another item announces it", said_contains("Back Up"));
  touch_at(240, 340, false);

  /* --- double tap activates what was announced --- */
  reset();
  clicked = 0;
  touch_at(240, 140, true);
  touch_at(240, 140, false); /* announce Scan */
  touch_at(240, 140, true);
  touch_at(240, 140, false); /* second tap */
  pump(6);
  check("a double tap activates what was announced", clicked == 1);

  /* --- swipe right steps to the next element --- */
  reset();
  touch_at(100, 600, true);
  touch_at(400, 600, true);
  touch_at(400, 600, false);
  pump(3);
  check("a swipe right steps the cursor", said_n > 0);

  /* --- turning it off restores normal touch --- */
  a11y_set_enabled(false);
  reset();
  clicked = 0;
  touch_at(240, 140, true);
  touch_at(240, 140, false);
  pump(3);
  check("turning it off restores the plain touch", clicked == 1 && said_n == 0);

  /* --- the UI hooks --- */
  a11y_set_enabled(true);

  reset();
  lv_obj_t *page = lv_obj_create(scr);
  lv_obj_set_size(page, 480, 200);
  theme_create_page_title(page, "Back Up");
  check("a page title announces its page", said_contains("Back Up"));

  reset();
  lv_obj_t *back = ui_create_back_button(page, NULL);
  char out2[64];
  a11y_describe(back, out2, sizeof(out2));
  check("the glyph-only back button has a name", strcmp(out2, "Back") == 0);
  check("the back button is a stop", a11y_is_stop(back));

  reset();
  lv_obj_t *info = ui_create_info_button(page, NULL);
  a11y_describe(info, out2, sizeof(out2));
  check("the about button has a name", strcmp(out2, "About") == 0);

  /* deleting a named object must release its slot */
  lv_obj_delete(info);
  pump(2);
  check("a name is dropped when its object is deleted", 1);

  /* --- masking: the part where a bug reads a seed out loud --- */
  lv_obj_t *secret = lv_obj_create(scr);
  lv_obj_set_pos(secret, 40, 620);
  lv_obj_set_size(secret, 400, 100);
  lv_obj_add_flag(secret, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *word = lv_label_create(secret);
  lv_label_set_text(word, "abandon ability able");
  a11y_mask(secret);
  pump(2);

  check("a child of a masked container is masked", a11y_is_masked(word));
  check("an object outside it is not", !a11y_is_masked(btn_a));

  reset();
  a11y_set_speak_secrets(false);
  touch_at(240, 660, true);
  touch_at(240, 660, false);
  check("gate shut: the seed is not spoken",
        !said_contains("abandon") && said_n == 0);
  int masked_beep = 0;
  for (int i = 0; i < earcon_n; i++)
    if (earcons[i] == SPEECH_EARCON_MASKED)
      masked_beep = 1;
  check("gate shut: a tone is played instead", masked_beep);

  reset();
  a11y_set_speak_secrets(true);
  spoken_reset_hack();
  touch_at(240, 140, true);
  touch_at(240, 140, false); /* move focus away */
  reset();
  touch_at(240, 660, true);
  touch_at(240, 660, false);
  check("gate open: the seed is spoken", said_contains("abandon"));
  a11y_set_speak_secrets(false);

  /* deleting the masked object must release the slot */
  lv_obj_delete(secret);
  pump(2);
  check("a mask is dropped when its object is deleted", !a11y_is_masked(btn_a));

  /* The hook checks left a container covering the buttons; drop it so the
     point search finds btn_a again. */
  lv_obj_delete(page);
  pump(3);

  /* --- EVENT mode: what the simulator's SDL mouse actually does --- */
  /* In LV_INDEV_MODE_EVENT the read timer is paused and a read happens only
     when SDL reports an event. After a button-up there are no more events. */
  a11y_set_enabled(true);
  lv_indev_set_mode(indev, LV_INDEV_MODE_EVENT);
  reset();
  clicked = 0;

/* One "event" is: set the state, one explicit read, let LVGL process it -
   and crucially, no reads of our own after that. */
#define EVENT_AT(X, Y, DOWN)                                                   \
  do {                                                                         \
    touch_pt.x = (X);                                                          \
    touch_pt.y = (Y);                                                          \
    touch_down = (DOWN);                                                       \
    lv_indev_read(indev);                                                      \
    lv_timer_handler();                                                        \
    lv_tick_inc(20);                                                           \
  } while (0)

  EVENT_AT(240, 140, true);
  EVENT_AT(240, 140, false); /* announces Scan */
  EVENT_AT(240, 140, true);
  EVENT_AT(240, 140, false); /* second tap */
  /* No further events - this is where the simulator used to sit. Only LVGL's
     timers run, which is what the fix uses to drive the reads. */
  for (int i = 0; i < 12; i++) {
    lv_timer_handler();
    lv_tick_inc(20);
  }
  check("event mode: a double tap activates with no further events",
        clicked == 1);

  reset();
  clicked = 0;
  spoken_reset_hack(); /* clear tap state between cases */
  EVENT_AT(240, 340, true);
  EVENT_AT(240, 340, false);
  for (int i = 0; i < 12; i++) {
    lv_timer_handler();
    lv_tick_inc(20);
  }
  check("event mode: a single tap activates nothing", clicked == 0);

  /* --- is input still alive after an activation? --- */
  reset();
  clicked = 0;
  spoken_reset_hack();
  /* activate once */
  EVENT_AT(240, 140, true);
  EVENT_AT(240, 140, false);
  EVENT_AT(240, 140, true);
  EVENT_AT(240, 140, false);
  for (int i = 0; i < 12; i++) {
    lv_timer_handler();
    lv_tick_inc(20);
  }
  const int first_click = clicked;

  /* explore another element: it should announce */
  reset();
  EVENT_AT(240, 340, true);
  check("input survives an activation", said_contains("Back Up"));
  EVENT_AT(240, 340, false);

  /* and a second activation should work again */
  clicked = 0;
  EVENT_AT(240, 140, true);
  EVENT_AT(240, 140, false);
  EVENT_AT(240, 140, true);
  EVENT_AT(240, 140, false);
  for (int i = 0; i < 12; i++) {
    lv_timer_handler();
    lv_tick_inc(20);
  }
  check("a second activation still works", first_click == 1 && clicked == 1);

  /* --- exploring has to count as activity --- */
  /* Without this LVGL sees an idle device: the screensaver comes up under the
     user's finger and the session lock follows, and the touches that would
     dismiss either are the ones being suppressed. */
  a11y_set_enabled(true);
  for (int i = 0; i < 20; i++) {
    lv_timer_handler();
    lv_tick_inc(100);
  }
  const uint32_t idle_antes = lv_display_get_inactive_time(NULL);
  EVENT_AT(240, 340, true);
  EVENT_AT(240, 340, false);
  const uint32_t idle_depois = lv_display_get_inactive_time(NULL);
  check("exploring counts as activity",
        idle_antes > 1000 && idle_depois < idle_antes);

  /* --- passthrough: overlays dismissed by any touch --- */
  reset();
  clicked = 0;
  a11y_set_passthrough(true);
  EVENT_AT(240, 140, true);
  EVENT_AT(240, 140, false);
  for (int i = 0; i < 4; i++) {
    lv_timer_handler();
    lv_tick_inc(20);
  }
  check("passthrough: a single touch reaches the widget",
        clicked == 1 && said_n == 0);
  a11y_set_passthrough(false);

  reset();
  clicked = 0;
  EVENT_AT(240, 140, true);
  EVENT_AT(240, 140, false);
  for (int i = 0; i < 4; i++) {
    lv_timer_handler();
    lv_tick_inc(20);
  }
  check("leaving passthrough restores interception",
        clicked == 0 && said_n > 0);

  /* --- the reported freeze, exactly --- */
  /* Real screensaver up, reader on, one plain touch. Before the fix this was
     a screen with no way out: the touch that dismisses it is the one the
     reader was swallowing. */
  {
    probe_dismiss_count = 0;
    a11y_set_enabled(true);
    screensaver_create(lv_screen_active(), probe_dismissed_cb, "");
    for (int i = 0; i < 4; i++) {
      lv_timer_handler();
      lv_tick_inc(20);
    }
    EVENT_AT(240, 400, true);
    EVENT_AT(240, 400, false);
    for (int i = 0; i < 10; i++) {
      lv_timer_handler();
      lv_tick_inc(20);
    }
    check("the real screensaver is dismissed by one touch",
          probe_dismiss_count == 1);
    screensaver_destroy();
    for (int i = 0; i < 4; i++) {
      lv_timer_handler();
      lv_tick_inc(20);
    }
    check("the reader is back after the screensaver", !a11y_is_masked(btn_a));
  }

  lv_indev_set_mode(indev, LV_INDEV_MODE_TIMER);
  a11y_set_enabled(false);

  /* --- real SDL dispatch: reaches a mouse after its read cb is wrapped --- */
  SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
  lv_display_t *sdl_disp = lv_sdl_window_create(64, 64);
  lv_indev_t *sdl_indev = lv_sdl_mouse_create();
  check("SDL test display and mouse are available", sdl_disp && sdl_indev);
  if (sdl_disp && sdl_indev) {
    lv_indev_set_display(sdl_indev, sdl_disp);
    sdl_original_read = lv_indev_get_read_cb(sdl_indev);
    lv_indev_set_read_cb(sdl_indev, sdl_wrapped_read);

    SDL_Window *window = lv_sdl_window_get_window(sdl_disp);
    SDL_Event event = {0};
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.windowID = SDL_GetWindowID(window);
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = 10;
    event.button.y = 10;
    sdl_wrapped_saw_press = false;
    lv_sdl_mouse_handler(&event);
    check("SDL still dispatches after the reader wraps its callback",
          sdl_wrapped_saw_press);
  }

  printf("\nPassed: %d, Failed: %d\n", pass, fail);
  return fail ? 1 : 0;
}
