#ifndef BB_JSON_UTILS_H
#define BB_JSON_UTILS_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static inline char *bb_json_get(const char *json, const char *key) {
    if (!json || !key) return NULL;

    char search[64];
    const size_t key_length = strlen(key);
    if (key_length > sizeof(search) - 3) return NULL;

    search[0] = '"';
    memcpy(search + 1, key, key_length);
    search[key_length + 1] = '"';
    search[key_length + 2] = '\0';

    const char *position = strstr(json, search);
    if (!position) return NULL;
    position += key_length + 2;

    while (*position == ' ' || *position == '\t' ||
           *position == '\r' || *position == '\n') {
        position++;
    }
    if (*position++ != ':') return NULL;
    while (*position == ' ' || *position == '\t' ||
           *position == '\r' || *position == '\n') {
        position++;
    }
    if (*position++ != '"') return NULL;

    char *result = malloc(strlen(position) + 1);
    if (!result) return NULL;
    char *output = result;

    while (*position && *position != '"') {
        unsigned char value = (unsigned char)*position++;
        if (value == '\\') {
            const char escaped = *position++;
            if (!escaped) {
                free(result);
                return NULL;
            }
            switch (escaped) {
                case '"': value = '"'; break;
                case '\\': value = '\\'; break;
                case '/': value = '/'; break;
                case 'b': value = '\b'; break;
                case 'f': value = '\f'; break;
                case 'n': value = '\n'; break;
                case 'r': value = '\r'; break;
                case 't': value = '\t'; break;
                default:
                    free(result);
                    return NULL;
            }
        } else if (value < 0x20) {
            free(result);
            return NULL;
        }
        *output++ = (char)value;
    }

    if (*position != '"') {
        free(result);
        return NULL;
    }
    *output = '\0';
    return result;
}

static inline bool bb_json_get_action(const char *json, char *action, size_t max_length) {
    if (!action || max_length == 0) return false;
    char *value = bb_json_get(json, "action");
    if (!value) return false;

    size_t length = strlen(value);
    if (length >= max_length) length = max_length - 1;
    memcpy(action, value, length);
    action[length] = '\0';
    free(value);
    return true;
}

static inline bool bb_json_has_action(const char *json, const char *action) {
    char value[64];
    return bb_json_get_action(json, value, sizeof(value)) &&
           strcmp(value, action) == 0;
}

#endif
