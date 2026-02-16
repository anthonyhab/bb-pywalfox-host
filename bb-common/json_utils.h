#ifndef BB_JSON_UTILS_H
#define BB_JSON_UTILS_H

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static inline char *bb_json_get(const char *json, const char *key) {
    char search[64];
    size_t key_len = strlen(key);
    if (key_len > 60) return NULL;
    
    search[0] = '"';
    memcpy(search + 1, key, key_len);
    search[key_len + 1] = '"';
    search[key_len + 2] = '\0';
    
    const char *pos = strstr(json, search);
    if (!pos) return NULL;
    
    pos = strchr(pos, ':');
    if (!pos) return NULL;
    pos++;
    
    while (*pos == ' ' || *pos == '\t') pos++;
    
    if (*pos == '"') {
        pos++;
        const char *end = strchr(pos, '"');
        if (!end) return NULL;
        size_t len = end - pos;
        char *result = (char *)malloc(len + 1);
        if (result) {
            memcpy(result, pos, len);
            result[len] = '\0';
        }
        return result;
    }
    
    return NULL;
}

static inline bool bb_json_get_action(const char *json, char *action, size_t max_len) {
    char *action_val = bb_json_get(json, "action");
    if (!action_val) return false;
    
    size_t len = strlen(action_val);
    if (len >= max_len) len = max_len - 1;
    memcpy(action, action_val, len);
    action[len] = '\0';
    free(action_val);
    return true;
}

static inline bool bb_json_has_action(const char *json, const char *action) {
    char buf[64];
    if (!bb_json_get_action(json, buf, sizeof(buf))) return false;
    return strcmp(buf, action) == 0;
}

#endif
