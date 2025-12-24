#ifndef WATCH_MAP_H
#define WATCH_MAP_H

#include <stddef.h>  // size_t

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    int wd;
    char *path;   
} Watch;

typedef struct {
    Watch *watches;
    size_t watches_count;
    size_t watches_capacity;
} WatchMap;

int   watch_ensure_capacity(WatchMap *map, size_t need);
int   watch_add(WatchMap *map, int wd, char *path);
Watch* watch_find(WatchMap *map, int wd);
void  watch_remove(WatchMap *map, int wd);
void  watch_free_all(WatchMap *map);

int   add_watch_tree(int notify_fd, WatchMap *map, const char *base_path);
void  watch_update_prefix(WatchMap *map, const char *old_path, const char *new_path);
void  watch_remove_subtree(int notify_fd, WatchMap *map, const char *prefix);

#endif
