#ifndef BB_PYWALFOX_CSS_MANAGER_H
#define BB_PYWALFOX_CSS_MANAGER_H

#include <stddef.h>

int css_enable(const char *target, char *message, size_t message_size);
int css_disable(const char *target, char *message, size_t message_size);
int css_set_font_size(const char *target, int size, char *message, size_t message_size);

#endif
