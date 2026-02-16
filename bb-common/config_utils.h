#ifndef BB_CONFIG_UTILS_H
#define BB_CONFIG_UTILS_H

#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define BB_CONFIG_DIR_SUFFIX "/.mozilla/native-messaging-hosts"
#define BB_CONFIG_PATH_MAX 512

static inline int bb_config_ensure(const char *exe_path, const char *name, const char *allowed_ext) {
    const char *home = getenv("HOME");
    if (!home) return -1;
    
    char config_dir[BB_CONFIG_PATH_MAX];
    char config_path[BB_CONFIG_PATH_MAX];
    
    snprintf(config_dir, sizeof(config_dir), "%s%s", home, BB_CONFIG_DIR_SUFFIX);
    snprintf(config_path, sizeof(config_path), "%s/%s.json", config_dir, name);
    
    struct stat st;
    if (stat(config_path, &st) == 0) return 0;
    
    if (mkdir(config_dir, 0700) != 0 && errno != EEXIST) return -1;
    
    FILE *f = fopen(config_path, "w");
    if (!f) return -1;
    
    fprintf(f, "{\n");
    fprintf(f, "  \"name\": \"%s\",\n", name);
    fprintf(f, "  \"description\": \"Native messaging host for %s\",\n", name);
    fprintf(f, "  \"path\": \"%s\",\n", exe_path);
    fprintf(f, "  \"type\": \"stdio\",\n");
    fprintf(f, "  \"allowed_extensions\": [\"%s\"]\n", allowed_ext);
    fprintf(f, "}\n");
    
    fclose(f);
    chmod(config_path, 0644);
    return 0;
}

static inline int bb_config_remove(const char *name) {
    const char *home = getenv("HOME");
    if (!home) return -1;
    
    char config_path[BB_CONFIG_PATH_MAX];
    snprintf(config_path, sizeof(config_path), "%s%s/%s.json", home, BB_CONFIG_DIR_SUFFIX, name);
    
    return unlink(config_path);
}

#endif
