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
 * Give `obj` a name to be read instead of whatever is inside it.
 *
 * For the controls that show only a glyph - the back, power, settings and
 * info buttons in the page corners - there is no text to derive a description
 * from, and a reader that says nothing for them leaves someone unable to find
 * their way back. `name` is stored by reference and must outlive the object;
 * a string literal is the intended use. The entry is dropped when the object
 * is deleted.
 */
void a11y_label(lv_obj_t *obj, const char *name);

/**
 * True when `obj` is something the reader should stop on, rather than a
 * container it should look inside. Buttons and other controls are stops; so is
 * a label with text. A panel holding them is not.
 */
bool a11y_is_stop(lv_obj_t *obj);

#endif /* DESCRIBE_H */
