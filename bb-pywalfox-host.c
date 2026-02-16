#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <fcntl.h>

#include "bb-common/native_messaging.h"
#include "bb-common/json_utils.h"
#include "bb-common/config_utils.h"

#define COLORS_PATH "/.cache/wal/colors.json"
#define EVENT_SIZE (sizeof(struct inotify_event))
#define BUF_SIZE (1024 * (EVENT_SIZE + 16))
#define SOCKET_PATH "/tmp/pywalfox_socket"

static void send_colors(char colors[][16], const char *wallpaper) {
    char json[2048];
    int pos = 0;
    
    pos += snprintf(json + pos, sizeof(json) - pos,
        "{\"action\":\"action:colors\",\"success\":true,\"data\":{\"colors\":[");
    for (int i = 0; i < 16; i++) {
        if (i) pos += snprintf(json + pos, sizeof(json) - pos, ",");
        pos += snprintf(json + pos, sizeof(json) - pos, "\"%s\"", colors[i]);
    }
    pos += snprintf(json + pos, sizeof(json) - pos, "],\"wallpaper\":\"%s\"}}", wallpaper);
    
    nm_send_message(json, (size_t)pos);
}

static void parse_and_send(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        nm_send_message("{\"success\":false,\"error\":\"no colors\"}", 34);
        return;
    }
    
    char json[8192];
    size_t len = fread(json, 1, sizeof(json) - 1, f);
    json[len] = '\0';
    fclose(f);
    
    char colors[16][16];
    for (int i = 0; i < 16; i++) {
        char key[8], *v;
        snprintf(key, sizeof(key), "color%d", i);
        if ((v = bb_json_get(json, key))) {
            size_t vlen = strlen(v);
            if (vlen > 15) vlen = 15;
            memcpy(colors[i], v, vlen);
            colors[i][vlen] = '\0';
            free(v);
        } else {
            memcpy(colors[i], "#000000", 8);
        }
    }
    
    static char wallpaper[512];
    char *w = bb_json_get(json, "wallpaper");
    if (w) {
        size_t wlen = strlen(w);
        if (wlen > 511) wlen = 511;
        memcpy(wallpaper, w, wlen);
        wallpaper[wlen] = '\0';
        free(w);
    } else {
        wallpaper[0] = '\0';
    }
    
    send_colors(colors, wallpaper);
}

static int create_socket(const char *path) {
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    size_t plen = strlen(path);
    if (plen > sizeof(addr.sun_path) - 1) plen = sizeof(addr.sun_path) - 1;
    memcpy(addr.sun_path, path, plen);
    addr.sun_path[plen] = '\0';
    
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    chmod(path, 0600);
    listen(fd, 5);
    return fd;
}

static void handle_client(int fd, const char *path) {
    uint32_t len;
    if (read(fd, &len, 4) != 4 || len > 1024) { close(fd); return; }
    
    char cmd[1025];
    if (read(fd, cmd, len) != (ssize_t)len) { close(fd); return; }
    cmd[len] = '\0';
    
    if (!strcmp(cmd, "action:update")) parse_and_send(path);
    else if (!strcmp(cmd, "theme:mode:dark")) nm_send_message("{\"action\":\"theme:mode\",\"data\":\"dark\"}", 33);
    else if (!strcmp(cmd, "theme:mode:light")) nm_send_message("{\"action\":\"theme:mode\",\"data\":\"light\"}", 34);
    else if (!strcmp(cmd, "theme:mode:auto")) nm_send_message("{\"action\":\"theme:mode\",\"data\":\"auto\"}", 33);
    
    close(fd);
}

static int send_cmd(const char *msg) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 1;
    
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return 1; }
    
    uint32_t len = (uint32_t)strlen(msg);
    write(fd, &len, 4);
    write(fd, msg, len);
    close(fd);
    return 0;
}

int main(int argc, char *argv[]) {
    char exe_path[512];
    realpath("/proc/self/exe", exe_path);
    
    const char *home = getenv("HOME");
    if (!home) { fprintf(stderr, "no HOME\n"); return 1; }
    
    char path[512];
    snprintf(path, sizeof(path), "%s%s", home, COLORS_PATH);
    fprintf(stderr, "path: %s\n", path);
    
    if (argc > 1) {
        if (!strcmp(argv[1], "install")) {
            bb_config_ensure(exe_path, "pywalfox", "pywalfox@frewacom.org");
            printf("Pywalfox config created at:\n  ~/.mozilla/native-messaging-hosts/pywalfox.json\n\nPlease restart Firefox.\n");
            return 0;
        }
        if (!strcmp(argv[1], "uninstall")) {
            bb_config_remove("pywalfox");
            printf("Uninstalled pywalfox manifest.\n");
            return 0;
        }
        if (!strcmp(argv[1], "update")) return send_cmd("action:update");
        if (!strcmp(argv[1], "dark")) return send_cmd("theme:mode:dark");
        if (!strcmp(argv[1], "light")) return send_cmd("theme:mode:light");
        if (!strcmp(argv[1], "auto")) return send_cmd("theme:mode:auto");
        if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
            printf("bb-pywalfox-host - Native messaging host\n\nUsage: bb-pywalfox-host [ACTION]\n\nActions:\n  start       Start daemon (default, for Firefox)\n  install     Install manifest\n  uninstall   Remove manifest\n  update      Reload colors\n  dark/light  Set theme mode\n");
            return 0;
        }
    }
    
    parse_and_send(path);
    
    int sock = create_socket(SOCKET_PATH);
    int inot = inotify_init();
    if (inot >= 0) inotify_add_watch(inot, path, IN_CLOSE_WRITE);
    
    char buf[BUF_SIZE];
    while (1) {
        fd_set fds;
        FD_ZERO(&fds);
        if (inot >= 0) FD_SET(inot, &fds);
        if (sock >= 0) FD_SET(sock, &fds);
        FD_SET(STDIN_FILENO, &fds);
        
        int max = STDIN_FILENO;
        if (inot > max) max = inot;
        if (sock > max) max = sock;
        
        if (select(max + 1, &fds, NULL, NULL, NULL) < 0) break;
        
        if (FD_ISSET(STDIN_FILENO, &fds)) {
            uint8_t msg[4096];
            if (nm_read_message(msg, sizeof(msg)) < 0) break;
        }
        if (sock >= 0 && FD_ISSET(sock, &fds)) {
            int c = accept(sock, NULL, NULL);
            if (c >= 0) handle_client(c, path);
        }
        if (inot >= 0 && FD_ISSET(inot, &fds)) {
            read(inot, buf, BUF_SIZE);
            parse_and_send(path);
        }
    }
    
    if (sock >= 0) { close(sock); unlink(SOCKET_PATH); }
    if (inot >= 0) close(inot);
    return 0;
}
