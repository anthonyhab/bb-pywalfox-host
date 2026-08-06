#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>

#include "bb-common/config_utils.h"
#include "bb-common/json_utils.h"
#include "bb-common/native_messaging.h"
#include "css_manager.h"

#define DEFAULT_COLORS_SUFFIX "/.cache/wal/colors.json"
#define DEFAULT_SOCKET_PATH "/tmp/pywalfox_socket"
#define HOST_VERSION "2.7.4"
#define CLIENT_READ_TIMEOUT_MS 5
#define WATCH_RETRY_MS 50
#define INOTIFY_BUFFER_SIZE (16 * 1024)
#define COLORS_FILE_SIZE (64 * 1024)
#define RESPONSE_SIZE 4096

static char last_colors_response[RESPONSE_SIZE];

static const char *get_socket_path(void) {
    const char *override = getenv("PYWALFOX_SOCKET_PATH");
    return override && override[0] ? override : DEFAULT_SOCKET_PATH;
}

static int get_colors_path(char *path, size_t size) {
    const char *override = getenv("PYWALFOX_COLORS_PATH");
    if (override && override[0]) {
        return snprintf(path, size, "%s", override) < (int)size ? 0 : -1;
    }

    const char *home = getenv("HOME");
    if (!home) {
        return -1;
    }
    return snprintf(path, size, "%s%s", home, DEFAULT_COLORS_SUFFIX) < (int)size ? 0 : -1;
}

static size_t json_escape(char *out, size_t capacity, const char *input) {
    size_t written = 0;
    for (const unsigned char *p = (const unsigned char *)input; *p; p++) {
        const char *escape = NULL;
        switch (*p) {
            case '\\': escape = "\\\\"; break;
            case '"': escape = "\\\""; break;
            case '\b': escape = "\\b"; break;
            case '\f': escape = "\\f"; break;
            case '\n': escape = "\\n"; break;
            case '\r': escape = "\\r"; break;
            case '\t': escape = "\\t"; break;
            default: break;
        }

        if (escape) {
            if (written + 2 >= capacity) break;
            out[written++] = escape[0];
            out[written++] = escape[1];
        } else if (*p >= 0x20) {
            if (written + 1 >= capacity) break;
            out[written++] = (char)*p;
        }
    }
    out[written] = '\0';
    return written;
}

static int valid_color(const char *color) {
    if (!color || strlen(color) != 7 || color[0] != '#') {
        return 0;
    }
    for (size_t i = 1; i < 7; i++) {
        const char c = color[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return 0;
        }
    }
    return 1;
}

static const char *const color_keys[16] = {
    "color0", "color1", "color2", "color3",
    "color4", "color5", "color6", "color7",
    "color8", "color9", "color10", "color11",
    "color12", "color13", "color14", "color15",
};

static int validate_relevant_keys(const char *source, size_t length) {
    int counts[16] = {0};

    for (size_t i = 0; i < length; i++) {
        if (source[i] != '"') continue;
        const size_t start = ++i;
        int escaped = 0;
        while (i < length && source[i] != '"') {
            if (source[i] == '\\') {
                escaped = 1;
                if (++i >= length) return -1;
            }
            i++;
        }
        if (i >= length) return -1;

        size_t next = i + 1;
        while (next < length &&
               (source[next] == ' ' || source[next] == '\t' ||
                source[next] == '\r' || source[next] == '\n')) {
            next++;
        }
        if (next >= length || source[next] != ':') continue;
        if (escaped) return -1;

        const size_t key_length = i - start;
        for (size_t key_index = 0; key_index < 16; key_index++) {
            if (strlen(color_keys[key_index]) == key_length &&
                memcmp(source + start, color_keys[key_index], key_length) == 0 &&
                ++counts[key_index] > 1) {
                return -1;
            }
        }
    }
    return 0;
}

static int copy_object_color(
    struct json_object *object,
    const char *key,
    char target[8]
) {
    struct json_object *value = NULL;
    if (!object || !json_object_object_get_ex(object, key, &value) ||
        !json_object_is_type(value, json_type_string)) {
        return -1;
    }
    const char *color = json_object_get_string(value);
    if (!valid_color(color)) return -1;
    memcpy(target, color, 8);
    return 0;
}

static int read_colors_response(const char *path, char *response, size_t capacity) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        close(fd);
        return -1;
    }

    FILE *file = fdopen(fd, "r");
    if (!file) {
        close(fd);
        return -1;
    }

    char source[COLORS_FILE_SIZE];
    const size_t length = fread(source, 1, sizeof(source) - 1, file);
    const int read_error = ferror(file);
    fclose(file);
    if (read_error || length == sizeof(source) - 1) {
        return -1;
    }
    source[length] = '\0';
    if (validate_relevant_keys(source, length) != 0) return -1;

    struct json_tokener *tokener = json_tokener_new_ex(32);
    if (!tokener) return -1;
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
    struct json_object *root = json_tokener_parse_ex(tokener, source, (int)length);
    const enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t parsed_length = json_tokener_get_parse_end(tokener);
    while (parsed_length < length &&
           (source[parsed_length] == ' ' || source[parsed_length] == '\t' ||
            source[parsed_length] == '\r' || source[parsed_length] == '\n')) {
        parsed_length++;
    }
    json_tokener_free(tokener);
    if (parse_error != json_tokener_success || parsed_length != length ||
        !root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        return -1;
    }

    struct json_object *colors_object = NULL;
    if (!json_object_object_get_ex(root, "colors", &colors_object) ||
        !json_object_is_type(colors_object, json_type_object)) {
        json_object_put(root);
        return -1;
    }

    char colors[16][8];
    for (size_t i = 0; i < 16; i++) {
        if (copy_object_color(colors_object, color_keys[i], colors[i]) != 0) {
            json_object_put(root);
            return -1;
        }
    }

    char escaped_wallpaper[1024] = "";
    struct json_object *wallpaper_object = NULL;
    if (json_object_object_get_ex(root, "wallpaper", &wallpaper_object)) {
        if (!json_object_is_type(wallpaper_object, json_type_string)) {
            json_object_put(root);
            return -1;
        }
        json_escape(
            escaped_wallpaper,
            sizeof(escaped_wallpaper),
            json_object_get_string(wallpaper_object)
        );
    }
    json_object_put(root);

    size_t position = 0;
    int result = snprintf(
        response,
        capacity,
        "{\"action\":\"action:colors\",\"success\":true,\"data\":{\"colors\":["
    );
    if (result < 0 || (size_t)result >= capacity) return -1;
    position = (size_t)result;

    for (int i = 0; i < 16; i++) {
        result = snprintf(
            response + position,
            capacity - position,
            "%s\"%s\"",
            i ? "," : "",
            colors[i]
        );
        if (result < 0 || (size_t)result >= capacity - position) return -1;
        position += (size_t)result;
    }

    result = snprintf(
        response + position,
        capacity - position,
        "],\"wallpaper\":\"%s\"}}",
        escaped_wallpaper
    );
    if (result < 0 || (size_t)result >= capacity - position) return -1;
    return 0;
}

static void send_colors(const char *path, int force) {
    char response[RESPONSE_SIZE];
    if (read_colors_response(path, response, sizeof(response)) != 0) {
        if (force) {
            nm_send_str(
                "{\"action\":\"action:colors\",\"success\":false,"
                "\"error\":\"Unable to read colors.json\"}"
            );
        }
        return;
    }

    if (!force && strcmp(response, last_colors_response) == 0) {
        return;
    }

    snprintf(last_colors_response, sizeof(last_colors_response), "%s", response);
    nm_send_str(response);
}

static void send_invalid_action(const char *action) {
    char escaped_action[192];
    json_escape(escaped_action, sizeof(escaped_action), action ? action : "(missing)");

    char response[320];
    snprintf(
        response,
        sizeof(response),
        "{\"action\":\"action:invalid\",\"success\":false,"
        "\"error\":\"Unsupported action: %.160s\"}",
        escaped_action
    );
    nm_send_str(response);
}

static int json_get_int(const char *json, const char *key, int *value) {
    char search[64];
    const size_t key_length = strlen(key);
    if (key_length > sizeof(search) - 3) return -1;
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *position = strstr(json, search);
    if (!position) return -1;
    position += strlen(search);
    while (*position == ' ' || *position == '\t' || *position == '\r' || *position == '\n') {
        position++;
    }
    if (*position++ != ':') return -1;
    while (*position == ' ' || *position == '\t' || *position == '\r' || *position == '\n') {
        position++;
    }

    errno = 0;
    char *end = NULL;
    const long parsed = strtol(position, &end, 10);
    if (errno || end == position || parsed < INT_MIN || parsed > INT_MAX) return -1;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end != ',' && *end != '}') return -1;
    *value = (int)parsed;
    return 0;
}

static void send_css_target_response(
    const char *action,
    const char *target,
    int success,
    const char *message
) {
    char escaped_target[192];
    char escaped_message[320];
    json_escape(escaped_target, sizeof(escaped_target), target);
    json_escape(escaped_message, sizeof(escaped_message), message);

    char response[768];
    snprintf(
        response,
        sizeof(response),
        "{\"action\":\"%s\",\"success\":%s,\"data\":\"%s\",\"%s\":\"%s\"}",
        action,
        success ? "true" : "false",
        escaped_target,
        success ? "message" : "error",
        escaped_message
    );
    nm_send_str(response);
}

static void send_font_size_response(int size, int success, const char *message) {
    char escaped_message[320];
    json_escape(escaped_message, sizeof(escaped_message), message);

    char response[512];
    snprintf(
        response,
        sizeof(response),
        "{\"action\":\"css:font:size\",\"success\":%s,\"data\":%d,\"%s\":\"%s\"}",
        success ? "true" : "false",
        size,
        success ? "message" : "error",
        escaped_message
    );
    nm_send_str(response);
}

static void handle_css_action(const char *action, const char *message) {
    char *target = bb_json_get(message, "target");
    if (!target) {
        send_invalid_action(action);
        return;
    }

    char result_message[256];
    int result;
    if (strcmp(action, "css:enable") == 0) {
        result = css_enable(target, result_message, sizeof(result_message));
        send_css_target_response(action, target, result == 0, result_message);
    } else if (strcmp(action, "css:disable") == 0) {
        result = css_disable(target, result_message, sizeof(result_message));
        send_css_target_response(action, target, result == 0, result_message);
    } else {
        int size = 0;
        if (json_get_int(message, "size", &size) != 0) {
            free(target);
            send_invalid_action(action);
            return;
        }
        result = css_set_font_size(target, size, result_message, sizeof(result_message));
        send_font_size_response(size, result == 0, result_message);
    }
    free(target);
}

static void handle_native_message(const char *message, const char *colors_path) {
    char action[64];
    if (!bb_json_get_action(message, action, sizeof(action))) {
        send_invalid_action(NULL);
        return;
    }

    if (strcmp(action, "debug:version") == 0) {
        nm_send_str(
            "{\"action\":\"debug:version\",\"success\":true,"
            "\"data\":\"" HOST_VERSION "\"}"
        );
    } else if (strcmp(action, "action:colors") == 0) {
        send_colors(colors_path, 1);
    } else if (strcmp(action, "css:enable") == 0 ||
               strcmp(action, "css:disable") == 0 ||
               strcmp(action, "css:font:size") == 0) {
        handle_css_action(action, message);
    } else {
        send_invalid_action(action);
    }
}

static int64_t monotonic_milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int read_all_before(int fd, void *buffer, size_t length, int64_t deadline) {
    size_t offset = 0;
    while (offset < length) {
        const int64_t now = monotonic_milliseconds();
        if (now < 0 || now >= deadline) return -1;

        const ssize_t count = read(fd, (char *)buffer + offset, length - offset);
        if (count > 0) {
            offset += (size_t)count;
            continue;
        }
        if (count == 0) return -1;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;

        struct pollfd descriptor = { .fd = fd, .events = POLLIN };
        const int ready = poll(&descriptor, 1, (int)(deadline - now));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || !(descriptor.revents & (POLLIN | POLLHUP))) return -1;
    }
    return 0;
}

static int write_all(int fd, const void *buffer, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        const ssize_t count = write(fd, (const char *)buffer + offset, length - offset);
        if (count > 0) {
            offset += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

static int create_socket(const char *path) {
    if (!path) return -1;
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(address.sun_path)) {
        close(fd);
        return -1;
    }
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);

    unlink(path);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        chmod(path, 0600) != 0 ||
        listen(fd, 4) != 0) {
        close(fd);
        unlink(path);
        return -1;
    }

    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

static void send_theme_mode(const char *mode) {
    char response[96];
    snprintf(
        response,
        sizeof(response),
        "{\"action\":\"theme:mode\",\"success\":true,\"data\":\"%s\"}",
        mode
    );
    nm_send_str(response);
}

static void handle_client(int fd, const char *colors_path) {
    const int64_t now = monotonic_milliseconds();
    if (now < 0) {
        close(fd);
        return;
    }
    const int64_t deadline = now + CLIENT_READ_TIMEOUT_MS;

    uint32_t length;
    if (read_all_before(fd, &length, sizeof(length), deadline) != 0 ||
        length == 0 || length > 1024) {
        close(fd);
        return;
    }

    char command[1025];
    if (read_all_before(fd, command, length, deadline) != 0) {
        close(fd);
        return;
    }
    command[length] = '\0';

    if (strcmp(command, "action:update") == 0) {
        send_colors(colors_path, 0);
    } else if (strcmp(command, "theme:mode:dark") == 0) {
        send_theme_mode("dark");
    } else if (strcmp(command, "theme:mode:light") == 0) {
        send_theme_mode("light");
    } else if (strcmp(command, "theme:mode:auto") == 0) {
        send_theme_mode("auto");
    }
    close(fd);
}

static int send_command(const char *message) {
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 1;

    const char *socket_path = get_socket_path();
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(socket_path) >= sizeof(address.sun_path)) {
        close(fd);
        return 1;
    }
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);

    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return 1;
    }

    const uint32_t length = (uint32_t)strlen(message);
    const int failed = write_all(fd, &length, sizeof(length)) != 0 ||
                       write_all(fd, message, length) != 0;
    close(fd);
    return failed ? 1 : 0;
}

static int setup_inotify(const char *colors_path, char *filename, size_t filename_size) {
    char directory[PATH_MAX];
    if (snprintf(directory, sizeof(directory), "%s", colors_path) >= (int)sizeof(directory)) {
        return -1;
    }

    char *separator = strrchr(directory, '/');
    if (!separator || !separator[1]) return -1;
    snprintf(filename, filename_size, "%s", separator + 1);
    *separator = '\0';

    const int fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (fd < 0) return -1;
    if (inotify_add_watch(
            fd,
            directory,
            IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE |
                IN_DELETE_SELF | IN_MOVE_SELF
        ) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int handle_inotify(int fd, const char *filename, const char *colors_path) {
    char buffer[INOTIFY_BUFFER_SIZE];
    int should_send = 0;
    int watch_lost = 0;

    for (;;) {
        const ssize_t length = read(fd, buffer, sizeof(buffer));
        if (length < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) break;
            return -1;
        }
        if (length == 0) break;

        size_t offset = 0;
        while (offset < (size_t)length) {
            const struct inotify_event *event =
                (const struct inotify_event *)(buffer + offset);
            if (event->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF | IN_Q_OVERFLOW)) {
                watch_lost = 1;
            }
            if (event->len && strcmp(event->name, filename) == 0) {
                should_send = 1;
            }
            offset += sizeof(*event) + event->len;
        }
    }

    if (should_send) send_colors(colors_path, 0);
    return watch_lost ? -1 : 0;
}

static int install_manifest(const char *executable_path) {
    const char *allowed_extensions[] = {
        "pywalfox@bb.hab.rip",
        "pywalfox@frewacom.org",
    };
    const int result = bb_config_ensure_many(
        executable_path,
        "pywalfox",
        allowed_extensions,
        sizeof(allowed_extensions) / sizeof(allowed_extensions[0])
    );
    if (result == 0) {
        printf("Pywalfox native messaging manifest installed. Restart Firefox.\n");
    }
    return result == 0 ? 0 : 1;
}

int main(int argc, char *argv[]) {
    char colors_path[PATH_MAX];
    if (get_colors_path(colors_path, sizeof(colors_path)) != 0) {
        fprintf(stderr, "Unable to determine colors.json path\n");
        return 1;
    }

    if (argc > 1 && strcmp(argv[1], "start") != 0) {
        if (strcmp(argv[1], "install") == 0) {
            char executable_path[PATH_MAX];
            if (!realpath("/proc/self/exe", executable_path)) return 1;
            return install_manifest(executable_path);
        }
        if (strcmp(argv[1], "uninstall") == 0) {
            return bb_config_remove("pywalfox") == 0 ? 0 : 1;
        }
        if (strcmp(argv[1], "update") == 0) return send_command("action:update");
        if (strcmp(argv[1], "dark") == 0) return send_command("theme:mode:dark");
        if (strcmp(argv[1], "light") == 0) return send_command("theme:mode:light");
        if (strcmp(argv[1], "auto") == 0) return send_command("theme:mode:auto");
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            printf(
                "bb-pywalfox-host - Native messaging host\n\n"
                "Usage: bb-pywalfox-host [start|install|uninstall|update|dark|light|auto]\n"
            );
            return 0;
        }
        // Firefox passes the connecting extension origin as an argument.
        // Unrecognized arguments therefore enter native-host mode.
    }

    const char *socket_path = get_socket_path();
    const int socket_fd = create_socket(socket_path);
    char watched_filename[NAME_MAX + 1] = "";
    int inotify_fd = setup_inotify(
        colors_path,
        watched_filename,
        sizeof(watched_filename)
    );
    int watch_needs_sync = inotify_fd < 0;

    for (;;) {
        if (inotify_fd < 0) {
            inotify_fd = setup_inotify(
                colors_path,
                watched_filename,
                sizeof(watched_filename)
            );
            if (inotify_fd >= 0 && watch_needs_sync) {
                if (access(colors_path, R_OK) == 0) send_colors(colors_path, 0);
                watch_needs_sync = 0;
            }
        }
        fd_set descriptors;
        FD_ZERO(&descriptors);
        FD_SET(STDIN_FILENO, &descriptors);
        int maximum = STDIN_FILENO;

        if (socket_fd >= 0) {
            FD_SET(socket_fd, &descriptors);
            if (socket_fd > maximum) maximum = socket_fd;
        }
        if (inotify_fd >= 0) {
            FD_SET(inotify_fd, &descriptors);
            if (inotify_fd > maximum) maximum = inotify_fd;
        }

        struct timeval retry_timeout = {
            .tv_sec = 0,
            .tv_usec = WATCH_RETRY_MS * 1000,
        };
        struct timeval *timeout = inotify_fd < 0 ? &retry_timeout : NULL;
        if (select(maximum + 1, &descriptors, NULL, NULL, timeout) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (FD_ISSET(STDIN_FILENO, &descriptors)) {
            uint8_t message[MAX_MSG_SIZE + 1];
            const int length = nm_read_message(message, sizeof(message));
            if (length < 0) break;
            handle_native_message((const char *)message, colors_path);
        }
        if (socket_fd >= 0 && FD_ISSET(socket_fd, &descriptors)) {
            const int client = accept4(
                socket_fd,
                NULL,
                NULL,
                SOCK_CLOEXEC | SOCK_NONBLOCK
            );
            if (client >= 0) handle_client(client, colors_path);
        }
        if (inotify_fd >= 0 && FD_ISSET(inotify_fd, &descriptors) &&
            handle_inotify(inotify_fd, watched_filename, colors_path) != 0) {
            close(inotify_fd);
            inotify_fd = -1;
            watch_needs_sync = 1;
        }
    }

    if (socket_fd >= 0) {
        close(socket_fd);
        unlink(socket_path);
    }
    if (inotify_fd >= 0) close(inotify_fd);
    return 0;
}
