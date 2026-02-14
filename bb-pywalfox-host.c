#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <stdint.h>

#define COLORS_PATH_SUFFIX "/.cache/wal/colors"
#define CONFIG_DIR_SUFFIX "/.mozilla/native-messaging-hosts"
#define EVENT_SIZE (sizeof(struct inotify_event))
#define BUF_SIZE (1024 * (EVENT_SIZE + 16))
#define MAX_PATH 512
#define MAX_JSON_SIZE 8192

static void ensure_config_exists(const char *exe_path) {
    char config_dir[MAX_PATH];
    char config_path[MAX_PATH];
    const char *home = getenv("HOME");
    
    if (!home) return;
    
    snprintf(config_dir, sizeof(config_dir), "%s%s", home, CONFIG_DIR_SUFFIX);
    snprintf(config_path, sizeof(config_path), "%s/pywalfox.json", config_dir);
    
    struct stat st;
    if (stat(config_path, &st) == 0) return;
    
    mkdir(config_dir, 0700);
    
    FILE *f = fopen(config_path, "w");
    if (!f) return;
    
    fprintf(f, "{\n");
    fprintf(f, "  \"name\": \"pywalfox\",\n");
    fprintf(f, "  \"description\": \"Native messaging host for Pywalfox\",\n");
    fprintf(f, "  \"path\": \"%s\",\n", exe_path);
    fprintf(f, "  \"type\": \"stdio\",\n");
    fprintf(f, "  \"allowed_extensions\": [\"pywalfox@frewacom.org\"]\n");
    fprintf(f, "}\n");
    
    fclose(f);
    chmod(config_path, 0644);
}

static char *find_json_value(const char *json, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    
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
        char *result = malloc(len + 1);
        if (result) {
            strncpy(result, pos, len);
            result[len] = '\0';
        }
        return result;
    }
    
    return NULL;
}

static void send_theme(const char *bg, const char *fg, const char *accent) {
    char json[512];
    int len = snprintf(json, sizeof(json),
        "{\"action\":\"colors\",\"success\":true,\"data\":{"
        "\"background\":\"%s\","
        "\"foreground\":\"%s\","
        "\"accent\":\"%s\"}}",
        bg, fg, accent);

    uint32_t msg_len = len;
    fwrite(&msg_len, 4, 1, stdout);
    fwrite(json, 1, len, stdout);
    fflush(stdout);
}

static void parse_and_send(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    
    char json[MAX_JSON_SIZE];
    size_t len = fread(json, 1, MAX_JSON_SIZE - 1, f);
    json[len] = '\0';
    fclose(f);
    
    char bg[32] = "#000000";
    char fg[32] = "#ffffff";
    char accent[32] = "#ffffff";
    
    if (strstr(json, "\"special\"")) {
        char *bg_val = find_json_value(json, "background");
        char *fg_val = find_json_value(json, "foreground");
        char *cursor_val = find_json_value(json, "cursor");
        
        if (bg_val) { strncpy(bg, bg_val, 31); free(bg_val); }
        if (fg_val) { strncpy(fg, fg_val, 31); free(fg_val); }
        if (cursor_val && strlen(cursor_val) > 0) {
            strncpy(accent, cursor_val, 31);
            free(cursor_val);
        } else {
            char *c2 = find_json_value(json, "color2");
            if (c2) { strncpy(accent, c2, 31); free(c2); }
        }
    }
    else if (strstr(json, "\"colors\"")) {
        char *c0 = find_json_value(json, "color0");
        char *c7 = find_json_value(json, "color7");
        char *c2 = find_json_value(json, "color2");
        
        if (c0) { strncpy(bg, c0, 31); free(c0); }
        if (c7) { strncpy(fg, c7, 31); free(c7); }
        if (c2) { strncpy(accent, c2, 31); free(c2); }
    }
    
    send_theme(bg, fg, accent);
}

int main(int argc, char *argv[]) {
    char exe_path[MAX_PATH];
    realpath("/proc/self/exe", exe_path);
    
    const char *home = getenv("HOME");
    if (!home) return 1;
    
    if (argc > 1 && strcmp(argv[1], "--setup") == 0) {
        ensure_config_exists(exe_path);
        printf("Pywalfox config created at:\n");
        printf("  ~/.mozilla/native-messaging-hosts/pywalfox.json\n");
        printf("\nPlease restart Firefox to apply changes.\n");
        return 0;
    }
    
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", home, COLORS_PATH_SUFFIX);

    // Wait for request from Pywalfox before sending initial colors
    uint32_t req_len;
    if (fread(&req_len, 4, 1, stdin) == 1 && req_len < 1024) {
        char req[1024];
        if (fread(req, 1, req_len, stdin) == req_len) {
            req[req_len] = '\0';
            // Check if Pywalfox requested colors
            if (strstr(req, "\"action\"") && strstr(req, "\"colors\"")) {
                parse_and_send(path);
            }
        }
    }

    // Set up inotify for file change notifications
    int fd = inotify_init();
    if (fd < 0) return 1;

    int wd = inotify_add_watch(fd, path, IN_MODIFY | IN_CLOSE_WRITE);
    if (wd < 0) return 1;

    // Main loop: watch for file changes and send updates
    char buf[BUF_SIZE];
    fd_set readfds;
    
    while (1) {
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);
        
        int max_fd = (fd > STDIN_FILENO) ? fd : STDIN_FILENO;
        
        if (select(max_fd + 1, &readfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        
        // Check for file changes
        if (FD_ISSET(fd, &readfds)) {
            int len = read(fd, buf, BUF_SIZE);
            if (len <= 0) break;
            parse_and_send(path);
        }
        
        // Check for additional requests from Pywalfox
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            uint32_t msg_len;
            if (fread(&msg_len, 4, 1, stdin) == 1 && msg_len < 1024) {
                char msg[1024];
                if (fread(msg, 1, msg_len, stdin) == msg_len) {
                    msg[msg_len] = '\0';
                    if (strstr(msg, "\"action\"") && strstr(msg, "\"colors\"")) {
                        parse_and_send(path);
                    }
                }
            }
        }
    }

    return 0;
}
