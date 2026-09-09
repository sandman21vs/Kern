#ifndef A11Y_SETTINGS_H
#define A11Y_SETTINGS_H

#include <lvgl.h>

void a11y_settings_page_create(lv_obj_t *parent, void (*return_cb)(void));
void a11y_settings_page_show(void);
void a11y_settings_page_hide(void);
void a11y_settings_page_destroy(void);

#endif /* A11Y_SETTINGS_H */
