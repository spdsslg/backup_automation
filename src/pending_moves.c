#define _GNU_SOURCE
#include "pending_moves.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

#include "filesystem_utils.h"
#include "watch_map.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef PENDING_MAX
#define PENDING_MAX 128
#endif


// helpers for pending move management
void pending_move_add(PendingMoves *pm, uint32_t cookie, int is_dir,
                      const char *src_old, const char *dst_old) {
  if (pm->pending_count == PENDING_MAX) {
    size_t oldest = 0;
    for (size_t i = 0; i < pm->pending_count; i++) {
      if (pm->pending[i].t < pm->pending[oldest].t) {
        oldest = i;
      }
    }

    pm->pending[oldest] = pm->pending[pm->pending_count - 1];
    pm->pending_count--;
  }

  PendingMove *new_move = &pm->pending[pm->pending_count++];
  new_move->cookie = cookie;
  new_move->is_dir = is_dir;
  new_move->t = time(NULL);
  snprintf(new_move->src_old, PATH_MAX, "%s", src_old);
  snprintf(new_move->dst_old, PATH_MAX, "%s", dst_old);
}

int pm_take(PendingMoves *pm, uint32_t cookie, PendingMove *out) {
  for (size_t i = 0; i < pm->pending_count; i++) {
    if (pm->pending[i].cookie == cookie) {
      *out = pm->pending[i];
      pm->pending[i] = pm->pending[pm->pending_count - 1];
      pm->pending_count--;
      return 1;
    }
  }

  return 0;
}

void pm_1s_expire(PendingMoves *pm, int notify_fd, WatchMap *map) {
  time_t now = time(NULL);
  size_t i = 0;
  while (i < pm->pending_count) {
    if (now - pm->pending[i].t < 1) {
      i++;
      continue;
    }

    rm_tree(pm->pending[i].dst_old);
    if (pm->pending[i].is_dir) {
      watch_remove_subtree(notify_fd, map, pm->pending[i].src_old);
    }

    pm->pending[i] = pm->pending[pm->pending_count - 1];
    pm->pending_count--;
  }
}

