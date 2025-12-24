#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>

#include "monitor.h"
#include "watch_map.h"
#include "mirror.h"
#include "pending_moves.h"
#include "filesystem_utils.h"
#include "config.h"

int monitor_and_mirror(const char *src_real, const char *dst_real, volatile sig_atomic_t* stop_flag) {
  int ifd = inotify_init();
  if (ifd < 0) {
    perror("inotify_init");
    exit(EXIT_FAILURE);
  }

  WatchMap map = {0};
  if (add_watch_tree(ifd, &map, src_real) < 0) {
    close(ifd);
    return -1;
  }

  PendingMoves pm = {0};
  struct pollfd pfd = {ifd, POLLIN, 0};

  char buffer[4096];
  while (!(*stop_flag)) {
    pm_1s_expire(&pm, ifd, &map);

    int poll_return = poll(&pfd, 1, 100);
    if(poll_return<0){
      if(errno == EINTR){
        continue;
      }
      break;
    }
    if(poll_return==0){
      continue;
    }

    ssize_t len = read(ifd, buffer, sizeof(buffer));
    if (len < 0) {
      if (errno == EINTR)
        continue;
      break;
    }

    ssize_t i = 0;
    while (i < len) {
      struct inotify_event *event = (struct inotify_event *)&buffer[i];
      i += (ssize_t)sizeof(*event) + (ssize_t)event->len;

      Watch *watch = watch_find(&map, event->wd);
      if (!watch)
        continue;

      if (event->mask & IN_IGNORED) {
        // watch was removed by the kernel
        watch_remove(&map, event->wd);
        continue;
      }

      char src_path[PATH_MAX];
      if (event->len > 0)
        snprintf(src_path, PATH_MAX, "%s/%s", watch->path, event->name);
      else
        snprintf(src_path, PATH_MAX, "%s", watch->path);

      char dst_path[PATH_MAX];
      if (map_src_to_dst(src_real, dst_real, src_path, dst_path) < 0) {
        continue;
      }

      int is_dir = (event->mask & IN_ISDIR) != 0;

      // root deleted/moved
      if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) &&
          strcmp(src_path, src_real) == 0) {
        *stop_flag = 1;
        break;
      }

      if (event->mask & IN_MOVED_FROM) {
        pending_move_add(&pm, event->cookie, is_dir, src_path, dst_path);
        continue;
      }

      if (event->mask & IN_MOVED_TO) {
        PendingMove mv;
        if (pm_take(&pm, event->cookie, &mv)) { // if it is a pair
          if (ensure_parent_dir(dst_path) < 0) {
            continue;
          }
          rename(mv.dst_old, dst_path);
          if (mv.is_dir) {
            // update watch paths for all watches under that directory
            watch_update_prefix(&map, mv.src_old, src_path);
          }
        }

        else {
          if (is_dir) {
            mirror_create_or_update(src_path, dst_path, src_real, dst_real, stop_flag);
            add_watch_tree(ifd, &map, src_path);
            copy_tree(src_path, dst_path, src_real, dst_real, stop_flag);
          } else {
            mirror_create_or_update(src_path, dst_path, src_real, dst_real, stop_flag);
          }
        }
        continue;
      }

      if (event->mask & IN_CREATE) {
        if (is_dir) {
          mirror_create_or_update(src_path, dst_path, src_real, dst_real, stop_flag);
          add_watch_tree(ifd, &map, src_path);
          copy_tree(src_path, dst_path, (char *)src_real, (char *)dst_real, stop_flag);
        } else {
          struct stat st;
          if (lstat(src_path, &st) == 0 && S_ISLNK(st.st_mode)) {
            mirror_create_or_update(src_path, dst_path, src_real, dst_real, stop_flag);
          }
        }
        continue;
      }

      if ((event->mask & IN_CLOSE_WRITE) && !is_dir) {
        mirror_create_or_update(src_path, dst_path, src_real, dst_real, stop_flag);
        continue;
      }

      if (event->mask & IN_DELETE) {
        mirror_delete_path(dst_path);
        if (is_dir)
          watch_remove_subtree(ifd, &map, src_path);
      }
    }
  }

  close(ifd);
  watch_free_all(&map);
  return 0;
}