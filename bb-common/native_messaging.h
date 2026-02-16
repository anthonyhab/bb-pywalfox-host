#ifndef BB_NATIVE_MESSAGING_H
#define BB_NATIVE_MESSAGING_H

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define MSG_LEN_SIZE 4
#define MAX_MSG_SIZE (64 * 1024)

static inline int nm_read_message(uint8_t *buf, size_t bufsize) {
    uint32_t msg_len;
    if (fread(&msg_len, MSG_LEN_SIZE, 1, stdin) != 1) {
        return -1;
    }
    if (msg_len == 0 || msg_len > MAX_MSG_SIZE || msg_len >= bufsize) {
        return -1;
    }
    if (fread(buf, 1, msg_len, stdin) != msg_len) {
        return -1;
    }
    buf[msg_len] = '\0';
    return (int)msg_len;
}

static inline void nm_send_message(const char *json, size_t len) {
    uint32_t msg_len = (uint32_t)len;
    fwrite(&msg_len, MSG_LEN_SIZE, 1, stdout);
    fwrite(json, 1, len, stdout);
    fflush(stdout);
}

static inline void nm_send_str(const char *json) {
    size_t len = 0;
    if (json) len = strlen(json);
    nm_send_message(json, len);
}

static inline bool nm_read_exact(uint8_t *buf, size_t n) {
    return fread(buf, 1, n, stdin) == n;
}

static inline bool nm_write_exact(const uint8_t *buf, size_t n) {
    return fwrite(buf, 1, n, stdout) == n;
}

#endif
