#define _GNU_SOURCE

#include "css_manager.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "assets_embedded.h"

#include <json-c/json.h>

#define MAX_CSS_SIZE (1024 * 1024)
#define BEGIN_MARKER "/******************** BEGIN PYWALFOX CUSTOM CSS ********************/"
#define END_MARKER "/********************* END PYWALFOX CUSTOM CSS *********************/"

struct css_asset {
    const char *filename;
    const unsigned char *data;
    size_t length;
};

static int get_asset(const char *target, struct css_asset *asset) {
    if (strcmp(target, "userChrome") == 0) {
        *asset = (struct css_asset){
            .filename = "userChrome.css",
            .data = user_chrome_css,
            .length = user_chrome_css_length,
        };
        return 0;
    }
    if (strcmp(target, "userContent") == 0) {
        *asset = (struct css_asset){
            .filename = "userContent.css",
            .data = user_content_css,
            .length = user_content_css_length,
        };
        return 0;
    }
    return -1;
}

static char *trim(char *value) {
    while (isspace((unsigned char)*value)) value++;
    char *end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return value;
}

static int get_profile_path(char *profile_path, size_t size) {
    const char *override = getenv("PYWALFOX_PROFILE_PATH");
    if (override && override[0]) {
        return snprintf(profile_path, size, "%s", override) < (int)size ? 0 : -1;
    }

    const char *home = getenv("HOME");
    if (!home) return -1;

    char ini_path[PATH_MAX];
    if (snprintf(ini_path, sizeof(ini_path), "%s/.mozilla/firefox/profiles.ini", home) >=
        (int)sizeof(ini_path)) {
        return -1;
    }

    FILE *file = fopen(ini_path, "r");
    if (!file) return -1;

    char *line = NULL;
    size_t capacity = 0;
    char section[64] = "";
    char install_default[64] = "";
    char named_default[64] = "";
    char default_section[64] = "";
    char first_profile[64] = "";

    while (getline(&line, &capacity, file) >= 0) {
        char *value = trim(line);
        if (!value[0] || value[0] == ';' || value[0] == '#') continue;
        const size_t length = strlen(value);
        if (value[0] == '[' && length > 2 && value[length - 1] == ']') {
            value[length - 1] = '\0';
            snprintf(section, sizeof(section), "%s", value + 1);
            continue;
        }
        if (!section[0]) continue;

        char *equals = strchr(value, '=');
        if (!equals) continue;
        *equals++ = '\0';
        const char *key = trim(value);
        const char *setting = trim(equals);
        if (strncmp(section, "Install", 7) == 0 && strcasecmp(key, "Default") == 0) {
            snprintf(install_default, sizeof(install_default), "%s", setting);
        } else if (strncmp(section, "Profile", 7) == 0) {
            if (!first_profile[0]) {
                snprintf(first_profile, sizeof(first_profile), "%s", section);
            }
            if (strcasecmp(key, "Name") == 0 &&
                install_default[0] && strcmp(setting, install_default) == 0) {
                if (!named_default[0]) {
                    snprintf(named_default, sizeof(named_default), "%s", section);
                }
            }
            /* [Install*] Default holds the profile DIRECTORY name, which is
             * the section's Path=, not its Name=. Match both; Path is the
             * authoritative one and wins when they differ. */
            if (strcasecmp(key, "Path") == 0 &&
                install_default[0] && strcmp(setting, install_default) == 0) {
                snprintf(named_default, sizeof(named_default), "%s", section);
            }
            if (strcasecmp(key, "Default") == 0 && strcmp(setting, "1") == 0) {
                snprintf(default_section, sizeof(default_section), "%s", section);
            }
        }
    }

    const int read_error = ferror(file);

    /* Resolution: [Install*] Default is either a [ProfileN] section or a
     * profile NAME (modern Firefox). Prefer the install-declared default,
     * then a legacy Default=1 profile, then the first profile section. */
    const char *selected = NULL;
    if (install_default[0]) {
        if (strncmp(install_default, "Profile", 7) == 0) {
            selected = install_default;
        } else if (named_default[0]) {
            selected = named_default;
        }
    }
    if (!selected) selected = default_section[0] ? default_section : NULL;
    if (!selected) selected = first_profile[0] ? first_profile : NULL;

    char configured_path[PATH_MAX] = "";
    int is_relative = 1;
    if (!selected || read_error || fseek(file, 0, SEEK_SET) != 0) {
        free(line);
        fclose(file);
        return -1;
    }

    while (getline(&line, &capacity, file) >= 0) {
        char *value = trim(line);
        if (!value[0] || value[0] == ';' || value[0] == '#') continue;
        const size_t length = strlen(value);
        if (value[0] == '[' && length > 2 && value[length - 1] == ']') {
            value[length - 1] = '\0';
            snprintf(section, sizeof(section), "%s", value + 1);
            continue;
        }
        if (strcmp(section, selected) != 0) continue;

        char *equals = strchr(value, '=');
        if (!equals) continue;
        *equals++ = '\0';
        const char *key = trim(value);
        const char *setting = trim(equals);
        if (strcasecmp(key, "Path") == 0) {
            snprintf(configured_path, sizeof(configured_path), "%s", setting);
        } else if (strcasecmp(key, "IsRelative") == 0) {
            is_relative = strcmp(setting, "1") == 0;
        }
    }

    free(line);
    const int second_read_error = ferror(file);
    fclose(file);
    if (second_read_error || !configured_path[0]) return -1;

    if (is_relative) {
        return snprintf(
            profile_path,
            size,
            "%s/.mozilla/firefox/%s",
            home,
            configured_path
        ) < (int)size ? 0 : -1;
    }
    return snprintf(profile_path, size, "%s", configured_path) < (int)size ? 0 : -1;
}

static int get_target_path(
    const char *target,
    struct css_asset *asset,
    char *target_path,
    size_t target_size,
    char *message,
    size_t message_size
) {
    if (get_asset(target, asset) != 0) {
        snprintf(message, message_size, "Invalid CSS target");
        return -1;
    }

    char profile_path[PATH_MAX];
    if (get_profile_path(profile_path, sizeof(profile_path)) != 0) {
        snprintf(message, message_size, "Could not determine Firefox profile path");
        return -1;
    }

    char chrome_path[PATH_MAX];
    if (snprintf(chrome_path, sizeof(chrome_path), "%s/chrome", profile_path) >=
        (int)sizeof(chrome_path)) {
        snprintf(message, message_size, "Firefox chrome path is too long");
        return -1;
    }
    if (mkdir(chrome_path, 0755) != 0 && errno != EEXIST) {
        snprintf(message, message_size, "Could not create Firefox chrome folder");
        return -1;
    }

    struct stat info;
    if (stat(chrome_path, &info) != 0 || !S_ISDIR(info.st_mode)) {
        snprintf(message, message_size, "Firefox chrome path is not a directory");
        return -1;
    }

    if (snprintf(target_path, target_size, "%s/%s", chrome_path, asset->filename) >=
        (int)target_size) {
        snprintf(message, message_size, "CSS target path is too long");
        return -1;
    }
    return 0;
}

static int read_file(const char *path, char **content, size_t *length) {
    FILE *file = fopen(path, "r");
    if (!file) return errno == ENOENT ? 1 : -1;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    const long file_size = ftell(file);
    if (file_size < 0 || file_size > MAX_CSS_SIZE || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }

    char *buffer = malloc((size_t)file_size + 1);
    if (!buffer) {
        fclose(file);
        return -1;
    }
    const size_t read_length = fread(buffer, 1, (size_t)file_size, file);
    int failed = ferror(file);
    if (fclose(file) != 0) failed = 1;
    if (failed || read_length != (size_t)file_size) {
        free(buffer);
        return -1;
    }
    buffer[read_length] = '\0';
    *content = buffer;
    *length = read_length;
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

static int write_atomic(const char *path, const void *content, size_t length) {
    char temporary[PATH_MAX];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid()) >=
        (int)sizeof(temporary)) {
        return -1;
    }

    mode_t mode = 0600;
    const int destination_fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (destination_fd >= 0) {
        struct stat destination_info;
        if (fstat(destination_fd, &destination_info) != 0) {
            close(destination_fd);
            return -1;
        }
        close(destination_fd);
        mode = destination_info.st_mode & 07777;
    } else if (errno != ENOENT) {
        return -1;
    }

    const int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (fchmod(fd, mode) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }

    int failed = write_all(fd, content, length) != 0;
    if (!failed && fsync(fd) != 0) failed = 1;
    if (close(fd) != 0) failed = 1;
    if (!failed && rename(temporary, path) != 0) failed = 1;
    if (!failed) {
        const char *slash = strrchr(path, '/');
        const size_t directory_length = slash ? (size_t)(slash - path) : 0;
        if (directory_length >= sizeof(temporary)) {
            failed = 1;
        } else {
            char directory[PATH_MAX];
            if (directory_length == 0) {
                snprintf(directory, sizeof(directory), ".");
            } else {
                memcpy(directory, path, directory_length);
                directory[directory_length] = '\0';
            }
            const int directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (directory_fd < 0 || fsync(directory_fd) != 0) failed = 1;
            if (directory_fd >= 0) close(directory_fd);
        }
    }
    if (failed) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int find_managed_block(
    char *content,
    char **begin,
    char **end,
    char *message,
    size_t message_size
) {
    *begin = strstr(content, BEGIN_MARKER);
    if (!*begin) {
        *end = NULL;
        return 0;
    }
    *end = strstr(*begin, END_MARKER);
    if (!*end) {
        snprintf(message, message_size, "Pywalfox CSS marker is incomplete; file was preserved");
        return -1;
    }
    *end += strlen(END_MARKER);
    if (**end == '\r') (*end)++;
    if (**end == '\n') (*end)++;
    return 1;
}

int css_enable(const char *target, char *message, size_t message_size) {
    struct css_asset asset;
    char target_path[PATH_MAX];
    if (get_target_path(
            target, &asset, target_path, sizeof(target_path), message, message_size
        ) != 0) {
        return -1;
    }

    char *existing = NULL;
    size_t existing_length = 0;
    const int read_result = read_file(target_path, &existing, &existing_length);
    if (read_result < 0) {
        snprintf(message, message_size, "Could not read existing CSS; file was preserved");
        return -1;
    }
    if (read_result == 1) {
        if (write_atomic(target_path, asset.data, asset.length) != 0) {
            snprintf(message, message_size, "Could not enable custom CSS");
            return -1;
        }
        snprintf(message, message_size, "%s enabled", asset.filename);
        return 0;
    }

    char *begin = NULL;
    char *end = NULL;
    const int block_result = find_managed_block(
        existing, &begin, &end, message, message_size
    );
    if (block_result < 0) {
        free(existing);
        return -1;
    }

    const size_t prefix_length = begin ? (size_t)(begin - existing) : existing_length;
    const size_t suffix_length = begin ? existing_length - (size_t)(end - existing) : 0;
    const int needs_newline = !begin && existing_length > 0 && existing[existing_length - 1] != '\n';
    const size_t output_length = prefix_length + (size_t)needs_newline +
                                 asset.length + suffix_length;
    char *output = malloc(output_length);
    if (!output) {
        free(existing);
        snprintf(message, message_size, "Out of memory while enabling custom CSS");
        return -1;
    }

    size_t position = 0;
    memcpy(output + position, existing, prefix_length);
    position += prefix_length;
    if (needs_newline) output[position++] = '\n';
    memcpy(output + position, asset.data, asset.length);
    position += asset.length;
    if (suffix_length) memcpy(output + position, end, suffix_length);

    const int result = write_atomic(target_path, output, output_length);
    free(output);
    free(existing);
    if (result != 0) {
        snprintf(message, message_size, "Could not enable custom CSS");
        return -1;
    }
    snprintf(message, message_size, "%s enabled", asset.filename);
    return 0;
}

int css_disable(const char *target, char *message, size_t message_size) {
    struct css_asset asset;
    char target_path[PATH_MAX];
    if (get_target_path(
            target, &asset, target_path, sizeof(target_path), message, message_size
        ) != 0) {
        return -1;
    }

    char *existing = NULL;
    size_t existing_length = 0;
    const int read_result = read_file(target_path, &existing, &existing_length);
    if (read_result < 0) {
        snprintf(message, message_size, "Could not read existing CSS; file was preserved");
        return -1;
    }
    if (read_result == 1) {
        snprintf(message, message_size, "%s was already disabled", asset.filename);
        return 0;
    }

    char *begin = NULL;
    char *end = NULL;
    const int block_result = find_managed_block(
        existing, &begin, &end, message, message_size
    );
    if (block_result < 0) {
        free(existing);
        return -1;
    }
    if (block_result == 0) {
        free(existing);
        snprintf(message, message_size, "%s was already disabled", asset.filename);
        return 0;
    }

    const size_t prefix_length = (size_t)(begin - existing);
    const size_t suffix_length = existing_length - (size_t)(end - existing);
    const size_t output_length = prefix_length + suffix_length;
    char *output = malloc(output_length + 1);
    if (!output) {
        free(existing);
        snprintf(message, message_size, "Out of memory while disabling custom CSS");
        return -1;
    }
    memcpy(output, existing, prefix_length);
    memcpy(output + prefix_length, end, suffix_length);
    output[output_length] = '\0';

    int only_whitespace = 1;
    for (size_t i = 0; i < output_length; i++) {
        if (!isspace((unsigned char)output[i])) {
            only_whitespace = 0;
            break;
        }
    }

    int result;
    if (only_whitespace) {
        result = unlink(target_path) == 0 || errno == ENOENT ? 0 : -1;
    } else {
        result = write_atomic(target_path, output, output_length);
    }
    free(output);
    free(existing);
    if (result != 0) {
        snprintf(message, message_size, "Could not disable custom CSS");
        return -1;
    }
    snprintf(message, message_size, "%s disabled", asset.filename);
    return 0;
}

int css_set_font_size(const char *target, int size, char *message, size_t message_size) {
    if (size < 8 || size > 72) {
        snprintf(message, message_size, "Font size must be between 8 and 72 pixels");
        return -1;
    }

    struct css_asset asset;
    char target_path[PATH_MAX];
    if (get_target_path(
            target, &asset, target_path, sizeof(target_path), message, message_size
        ) != 0) {
        return -1;
    }

    char *existing = NULL;
    size_t existing_length = 0;
    if (read_file(target_path, &existing, &existing_length) != 0) {
        snprintf(message, message_size, "Enable custom CSS before setting its font size");
        return -1;
    }

    char *begin = NULL;
    char *end = NULL;
    if (find_managed_block(existing, &begin, &end, message, message_size) != 1) {
        free(existing);
        snprintf(message, message_size, "Enable custom CSS before setting its font size");
        return -1;
    }

    char *property = strstr(begin, "--pywalfox-font-size:");
    if (!property || property >= end) {
        free(existing);
        snprintf(message, message_size, "Managed CSS does not contain a font-size property");
        return -1;
    }
    char *value_start = strchr(property, ':');
    char *value_end = value_start ? strchr(value_start, ';') : NULL;
    if (!value_start || !value_end || value_end >= end) {
        free(existing);
        snprintf(message, message_size, "Managed font-size property is invalid");
        return -1;
    }
    value_start++;
    while (value_start < value_end && isspace((unsigned char)*value_start)) value_start++;

    char replacement[32];
    const int replacement_length = snprintf(replacement, sizeof(replacement), "%dpx", size);
    const size_t prefix_length = (size_t)(value_start - existing);
    const size_t suffix_length = existing_length - (size_t)(value_end - existing);
    const size_t output_length = prefix_length + (size_t)replacement_length + suffix_length;
    char *output = malloc(output_length);
    if (!output) {
        free(existing);
        snprintf(message, message_size, "Out of memory while setting font size");
        return -1;
    }

    memcpy(output, existing, prefix_length);
    memcpy(output + prefix_length, replacement, (size_t)replacement_length);
    memcpy(
        output + prefix_length + (size_t)replacement_length,
        value_end,
        suffix_length
    );

    const int result = write_atomic(target_path, output, output_length);
    free(output);
    free(existing);
    if (result != 0) {
        snprintf(message, message_size, "Could not set font size");
        return -1;
    }
    snprintf(message, message_size, "Font size set to %d", size);
    return 0;
}

/* --- Boot-time page fallback -------------------------------------------
 * Kills the startup white flash: while the theme extensions are booting,
 * pages paint light. The host writes a dark palette fallback into the
 * profile's chrome dir (imported by userContent.css), removes it once the
 * extension signals it is up, and re-writes it at shutdown so the next
 * session starts covered. Scoped to documents darkreader has not tinted,
 * so it never fights the applied theme. */

static int fallback_chrome_path(char *chrome_path, size_t size) {
    char profile_path[PATH_MAX];
    if (get_profile_path(profile_path, sizeof(profile_path)) != 0) {
        return -1;
    }
    if (snprintf(chrome_path, size, "%s/chrome", profile_path) >= (int)size) return -1;
    if (mkdir(chrome_path, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int css_write_boot_fallback(const char *colors_path) {
    (void)colors_path; /* the inversion is palette-independent */

    char chrome_path[PATH_MAX];
    if (fallback_chrome_path(chrome_path, sizeof(chrome_path)) != 0) {
        return -1;
    }

    char output[1024];
    const int output_length = snprintf(
        output, sizeof(output),
        "/* Boot-time fallback palette — generated by bb-pywalfox-host.\n"
        " * Darkens pages until the theme extension tints them; scoped to\n"
        " * documents darkreader has not applied (removed once the\n"
        " * extension is up, rewritten at shutdown for the next session).\n"
        " * Page-wide inversion covers inner surfaces a background rule\n"
        " * cannot reach; media is counter-inverted to stay natural. */\n"
        "html:not([data-darkreader-scheme]) {\n"
        "    filter: invert(1) hue-rotate(180deg) !important;\n"
        "}\n"
        "html:not([data-darkreader-scheme]) img,\n"
        "html:not([data-darkreader-scheme]) picture,\n"
        "html:not([data-darkreader-scheme]) video,\n"
        "html:not([data-darkreader-scheme]) canvas {\n"
        "    filter: invert(1) hue-rotate(180deg) !important;\n"
        "}\n"
    );
    if (output_length < 0 || output_length >= (int)sizeof(output)) return -1;

    char target_path[PATH_MAX];
    if (snprintf(target_path, sizeof(target_path), "%s/palette-boot.css", chrome_path) >=
        (int)sizeof(target_path)) {
        return -1;
    }
    if (write_atomic(target_path, output, (size_t)output_length) != 0) {
    }
    return 0;
}

int css_remove_boot_fallback(void) {
    char chrome_path[PATH_MAX];
    if (fallback_chrome_path(chrome_path, sizeof(chrome_path)) != 0) {
    }

    char target_path[PATH_MAX];
    if (snprintf(target_path, sizeof(target_path), "%s/palette-boot.css", chrome_path) >=
        (int)sizeof(target_path)) {
        return -1;
    }
    if (unlink(target_path) != 0 && errno != ENOENT) return -1;
    return 0;
}
