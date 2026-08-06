#ifndef BB_NATIVE_MESSAGING_H
#define BB_NATIVE_MESSAGING_H

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define MSG_LEN_SIZE 4
#define MAX_MSG_SIZE (64 * 1024)

static inline bool nm_read_exact(uint8_t *buf, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        const ssize_t count = read(STDIN_FILENO, buf + offset, length - offset);
        if (count > 0) {
            offset += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static inline bool nm_write_exact(const uint8_t *buf, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        const ssize_t count = write(STDOUT_FILENO, buf + offset, length - offset);
        if (count > 0) {
            offset += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static inline int nm_read_message(uint8_t *buf, size_t bufsize) {
    uint8_t header[MSG_LEN_SIZE];
    if (!nm_read_exact(header, sizeof(header))) return -1;

    const uint32_t msg_len = (uint32_t)header[0] |
                             ((uint32_t)header[1] << 8) |
                             ((uint32_t)header[2] << 16) |
                             ((uint32_t)header[3] << 24);
    if (msg_len == 0 || msg_len > MAX_MSG_SIZE || msg_len >= bufsize) return -1;
    if (!nm_read_exact(buf, msg_len)) return -1;
    buf[msg_len] = '\0';
    return (int)msg_len;
}

static inline void nm_send_message(const char *json, size_t length) {
    const uint32_t msg_len = (uint32_t)length;
    const uint8_t header[MSG_LEN_SIZE] = {
        (uint8_t)(msg_len & 0xff),
        (uint8_t)((msg_len >> 8) & 0xff),
        (uint8_t)((msg_len >> 16) & 0xff),
        (uint8_t)((msg_len >> 24) & 0xff),
    };
    if (!nm_write_exact(header, sizeof(header))) return;
    if (length > 0) nm_write_exact((const uint8_t *)json, length);
}

static inline void nm_send_str(const char *json) {
    nm_send_message(json, json ? strlen(json) : 0);
}

#endif
