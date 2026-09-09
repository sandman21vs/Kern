// Turning a widget into the words that describe it

#ifndef DESCRIBE_H
#define DESCRIBE_H

#include <lvgl.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * Write what `obj` should sound like into `out`.
 *
 * A label is its text. A button is the text of the labels inside it, so an
 * entry built as icon-plus-text reads as its text. A switch gains "on" or
 * "off", a disabled control gains "dimmed".
 *
 * @return number of characters written; 0 when there is nothing to say, which
 *         is how the caller knows to keep looking.
 */
size_t a11y_describe(lv_obj_t *obj, char *out, size_t max);

/**
 * True when `obj` is something the reader should stop on, rather than a
 * container it should look inside. Buttons and other controls are stops; so is
 * a label with text. A panel holding them is not.
 */
bool a11y_is_stop(lv_obj_t *obj);

#endif /* DESCRIBE_H */
