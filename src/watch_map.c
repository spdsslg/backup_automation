#define _GNU_SOURCE
#include "watch_map.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include "filesystem_utils.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

int watch_ensure_capacity(WatchMap *map, size_t need) {
  if (map->watches_capacity >= need)
    return 0;
  size_t new_cap =
      (map->watches_capacity == 0) ? 64 : (map->watches_capacity * 2);
  while (new_cap < need)
    new_cap *= 2;

  Watch *new_watches = realloc(map->watches, new_cap * sizeof(*new_watches));
  if (!new_watches) {
    fprintf(stderr, "new_watches malloc failed\n");
    return -1;
  }

  map->watches = new_watches;
  map->watches_capacity = new_cap;
  return 0;
}

// some of the functions below were taken/modified from
// https://gitlab.com/SaQQ/sop1/-/blob/master/05_events/watch_tree.c?ref_type=heads
int watch_add(WatchMap *map, int wd, char *path) {
  if (watch_ensure_capacity(map, map->watches_count + 1) < 0) {
    fprintf(stderr, "watch add failed\n");
    return -1;
  }

  map->watches[map->watches_count].wd = wd;
  map->watches[map->watches_count].path = path;
  map->watches_count++;
  return 0;
}

Watch *watch_find(WatchMap *map, int wd) {
  for (size_t i = 0; i < map->watches_count; i++) {
    if (map->watches[i].wd == wd) {
      return &map->watches[i];
    }
  }
  return NULL;
}

void watch_remove(WatchMap *map, int wd) {
  for (size_t i = 0; i < map->watches_count; i++) {
    if (map->watches[i].wd == wd) {
      free(map->watches[i].path);
      map->watches[i] = map->watches[map->watches_count - 1];
      map->watches_count--;
      return;
    }
  }
}

void watch_free_all(WatchMap *map) {
  for (size_t i = 0; i < map->watches_count; i++) {
    free(map->watches[i].path);
  }
  free(map->watches);
  map->watches = NULL;
  map->watches_capacity = 0;
  map->watches_count = 0;
}

int add_watch_tree(int notify_fd, WatchMap *map, const char *base_path) {
  uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                  IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED;

  int wd = inotify_add_watch(notify_fd, base_path, mask);
  if (wd < 0) {
    perror("inotify_add_watch");
    return -1;
  }
  watch_add(map, wd, strdup(base_path));

  DIR *dir = opendir(base_path);
  if (!dir) {
    perror("opendir");
    return -1;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char child[PATH_MAX];
    if (snprintf(child, sizeof(child), "%s/%s", base_path, entry->d_name) >=
        PATH_MAX) {
      closedir(dir);
      fprintf(stderr, "full path name too long\n");
      return -1;
    }

    struct stat st;
    if (lstat(child, &st) < 0) {
      closedir(dir);
      return -1;
    }

    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
      if (add_watch_tree(notify_fd, map, child) < 0) {
        closedir(dir);
        return -1;
      }
    }
  }

  if (closedir(dir) < 0) {
    perror("closedir");
    return -1;
  }
  return 0;
}

void watch_update_prefix(WatchMap *map, const char *old_path,
                         const char *new_path) {
  size_t oldlen = strlen(old_path);

  for (size_t i = 0; i < map->watches_count; i++) {
    char *p = map->watches[i].path;
    if (!has_prefix_path(p, old_path)) {
      continue;
    }

    char *suffix = p + oldlen;
    if (*suffix == '/')
      suffix++;

    char buf[PATH_MAX];
    if (*suffix) {
      snprintf(buf, PATH_MAX, "%s/%s", new_path, suffix);
    } else {
      snprintf(buf, PATH_MAX, "%s", new_path);
    }

    free(map->watches[i].path);
    map->watches[i].path = strdup(buf);
  }
}

void watch_remove_subtree(int notify_fd, WatchMap *map, const char *prefix) {
  size_t i = 0;
  while (i < map->watches_count) {
    if (has_prefix_path(map->watches[i].path, prefix)) {
      inotify_rm_watch(notify_fd, map->watches[i].wd);
      watch_remove(map, map->watches[i].wd);
      continue;
    }
    i++;
  }
}