#include <sys/poll.h>
#include <cerrno>
#define _GNU_SOURCE
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

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_ARGS 32
#define PENDING_MAX 128

int copy_file(const char *src, const char *dst, mode_t mode);
int copy_symplink_rewrite(const char *src_link, const char *dst_link,
                          const char *src_real, const char *dst_real);
int mkdir_p(const char *path, mode_t mode);
int rm_tree(const char *path);
int has_prefix_path(const char *s, const char *prefix);
int copy_tree(const char *src_dir, const char *dst_dir, const char *src_real,
              const char *dst_real);

static volatile sig_atomic_t g_terminate = 0;
static volatile sig_atomic_t g_got_sigchld = 0;
static volatile sig_atomic_t g_child_exit = 0;

typedef struct {
  char *src;
  char *dst;
  pid_t pid;
  time_t created_at;
  int active;
} Backup;

typedef struct {
  Backup *backups;
  size_t backups_count;
  size_t backups_capacity;
} BackupList;

typedef struct {
  int wd;
  char *path;
} Watch;

typedef struct {
  Watch *watches;
  size_t watches_count;
  size_t watches_capacity;
} WatchMap;

typedef struct {
  uint32_t cookie;
  int is_dir;
  time_t t;
  char src_old[PATH_MAX];
  char dst_old[PATH_MAX];
} PendingMove;

typedef struct {
  PendingMove pending[PENDING_MAX];
  size_t pending_count;
} PendingMoves;

static BackupList g_list = {0};

static void on_parent_terminate(int sig) { g_terminate = 1; }

static void on_sigchld(int sig) { g_got_sigchld = 1; }

static void on_child_term(int sig) { g_child_exit = 1; }

static int sethandler(void (*f)(int), int sigNo) {
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = f;
  sigemptyset(&act.sa_mask);

  if (sigaction(sigNo, &act, NULL) < 0) {
    perror("sigaction");
    return -1;
  }
  return 0;
}

static void install_parent_signals(void) {
  sethandler(on_parent_terminate, SIGINT);
  sethandler(on_parent_terminate, SIGTERM);
  sethandler(on_sigchld, SIGCHLD);
}

static void child_install_signals(void) {
  if (sethandler(on_child_term, SIGTERM) < 0)
    _exit(1);
}

ssize_t bulk_read(int fd, char *buf, size_t count) {
  ssize_t c;
  ssize_t len = 0;
  do {
    c = TEMP_FAILURE_RETRY(read(fd, buf, count));
    if (c < 0)
      return c;
    if (c == 0)
      return len; // EOF
    buf += c;
    len += c;
    count -= (size_t)c;
  } while (count > 0);
  return len;
}

ssize_t bulk_write(int fd, char *buf, size_t count) {
  ssize_t c;
  ssize_t len = 0;
  do {
    c = TEMP_FAILURE_RETRY(write(fd, buf, count));
    if (c < 0)
      return c;
    buf += c;
    len += c;
    count -= (size_t)c;
  } while (count > 0);
  return len;
}

// parsing
int parse_line(char *line, char *argv[MAX_ARGS],
               char argv_buf[MAX_ARGS][PATH_MAX], int *argc_out) {
  int argc = 0;
  char *p = line;
  while (*p) {
    while (isspace((unsigned char)*p)) {
      p++;
    }
    if (*p == '\0' || *p == '\n') {
      break;
    }

    if (argc >= MAX_ARGS) {
      printf("Too many arguments\n");
      return -1;
    }

    char *out = argv_buf[argc];
    int out_len = 0;
    out[0] = '\0';
    if (*p == '\'' || *p == '"') {
      char q = *p;
      p++;
      while (*p && q != *p) {
        if (q == '"' && *p == '\\') {
          p++;
          if (*p == '\0') // string ended after backslash
          {
            printf("Unexpected \\ or quote in the end of the argument\n");
            return -1;
          }
          char ch = *p;
          if (ch == '"' || ch == '\\') { // supports only \\ and \"
            if (out_len + 1 >= PATH_MAX) {
              printf("Path is too big\n");
              return -1;
            }
            out[out_len++] = ch;
            out[out_len] = '\0';
            p++;
          } else {
            printf("Unexpected escape sequence! Program supports only \\ and "
                   "\" inside arguments\n");
            return -1;
          }
        } else { // copy normal character
          if (out_len >= PATH_MAX) {
            printf("Path is too big\n");
            return -1;
          }
          out[out_len++] = *p;
          out[out_len] = '\0';
          p++;
        }
      }
      if (*p != q) {
        printf("No closing quote found\n");
        return -1;
      }
      p++;
    } else {
      while (*p && !isspace((unsigned char)*p)) {
        if (out_len >= PATH_MAX) {
          printf("Path is too big\n");
          return -1;
        }
        out[out_len++] = *p;
        out[out_len] = '\0';
        p++;
      }
    }
    argv[argc++] = out;
  }

  *argc_out = argc;
  return 0;
}

// helpers for path normalisation
int norm_existing_dir(const char *in, char out[PATH_MAX]) {
  if (!realpath(in, out)) {
    perror("realpath(src)");
    return -1;
  }
  struct stat st;
  if (stat(out, &st) < 0) {
    perror("stat(src)");
    return -1;
  }
  if (!S_ISDIR(st.st_mode)) {
    fprintf(stderr, "src is not a directory\n");
    return -1;
  }
  return 0;
}

void split_dir_base(char *path, char dir[PATH_MAX], char base[PATH_MAX]) {
  // dirname/base
  size_t n = strlen(path);
  while (n > 1 && path[n - 1] == '/') {
    n--;
  }
  size_t i = n;
  while (i != 0 && path[i - 1] != '/') {
    i--;
  }
  // prefix (dir)
  if (i == 0) {
    snprintf(dir, PATH_MAX, ".");
  } else if (i == 1) {
    snprintf(dir, PATH_MAX, "/");
  } else {
    snprintf(dir, PATH_MAX, "%.*s", (int)(i - 1), path);
  }
  // suffix (base)
  snprintf(base, PATH_MAX, "%.*s", (int)(n - i), path + i);
}

int norm_target_path(char *in, char out[PATH_MAX]) {
  if (realpath(in, out)) {
    return 0;
  }

  // case when dir doesn't exist yet (dir/base) and we assume parent exists
  char directory[PATH_MAX], base[PATH_MAX];
  split_dir_base(in, directory, base);

  char directory_realpath[PATH_MAX];
  if (!realpath(directory, directory_realpath)) {
    perror("realpath(dst parent)");
    return -1;
  }
  if (snprintf(out, PATH_MAX, "%s/%s", directory_realpath, base) >= PATH_MAX) {
    fprintf(stderr, "dst path too long\n");
    return -1;
  }
  return 0;
}

// helpers for inotify mirroring
int map_src_to_dst(const char *src_real, const char *dst_real,
                   const char *src_path, char dst_path[PATH_MAX]) {
  size_t n = strlen(src_real);

  if (strncmp(src_path, src_real, n) != 0) {
    return -1;
  }
  if (src_path[n] != '\0' && src_path[n] != '/') {
    return -1;
  }

  const char *suffix = src_path + n;
  if (*suffix == '/') {
    suffix++;
  }

  if (*suffix == '\0') {
    if (snprintf(dst_path, PATH_MAX, "%s", dst_real) >= PATH_MAX) {
      fprintf(stderr, "dst path too long\n");
      return -1;
    }
    return 0;
  }

  if (snprintf(dst_path, PATH_MAX, "%s/%s", dst_real, suffix) >= PATH_MAX) {
    fprintf(stderr, "dst path too long\n");
    return -1;
  }
  return 0;
}

int ensure_parent_dir(const char *fullpath) {
  char tmp[PATH_MAX];
  if (snprintf(tmp, PATH_MAX, "%s", fullpath) >= PATH_MAX) {
    return -1;
  }

  char dir[PATH_MAX], base[PATH_MAX];
  split_dir_base(tmp, dir, base);

  if (strcmp(dir, ".") == 0 || strcmp(dir, "/") == 0) {
    return 0;
  }
  if (mkdir_p(dir, 0755) < 0) {
    return -1;
  }
  return 0;
}

int mirror_create_or_update(const char *src_path, const char *dst_path,
                            const char *src_real, const char *dst_real) {
  struct stat st;
  if (lstat(src_path, &st) < 0)
    return -1;

  if (ensure_parent_dir(dst_path) < 0)
    return -1;

  if (S_ISDIR(st.st_mode)) {
    if (mkdir(dst_path, st.st_mode & 0777) < 0 && errno != EEXIST)
      return -1;
    return 0;
  }

  if (S_ISREG(st.st_mode)) {
    return copy_file(src_path, dst_path, st.st_mode);
  }
  if (S_ISLNK(st.st_mode)) {
    return copy_symplink_rewrite(src_path, dst_path, src_real, dst_real);
  }

  return 0;
}

int mirror_delete_path(char *dst_path) { return rm_tree(dst_path); }

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

// mirroring itself
int monitor_and_mirror(const char *src_real, const char *dst_real) {
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
  while (!g_child_exit) {
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
        g_child_exit = 1;
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
            mirror_create_or_update(src_path, dst_path, src_real, dst_real);
            add_watch_tree(ifd, &map, src_path);
            copy_tree(src_path, dst_path, src_real, dst_real);
          } else {
            mirror_create_or_update(src_path, dst_path, src_real, dst_real);
          }
        }
        continue;
      }

      if (event->mask & IN_CREATE) {
        if (is_dir) {
          mirror_create_or_update(src_path, dst_path, src_real, dst_real);
          add_watch_tree(ifd, &map, src_path);
          copy_tree(src_path, dst_path, (char *)src_real, (char *)dst_real);
        } else {
          struct stat st;
          if (lstat(src_path, &st) == 0 && S_ISLNK(st.st_mode)) {
            mirror_create_or_update(src_path, dst_path, src_real, dst_real);
          }
        }
        continue;
      }

      if ((event->mask & IN_CLOSE_WRITE) && !is_dir) {
        mirror_create_or_update(src_path, dst_path, src_real, dst_real);
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

// restoring helpers
int check_src_against_backup(const char *src_path, const char *backup_path) {
  struct stat backup_st;
  if (lstat(backup_path, &backup_st) <
      0) { // if backup doesn't have smth, delete it from src
    if (errno == ENOENT) {
      return rm_tree(src_path); // rm_tree handles non-existent files
    }
    perror("lstat(check_src_against_backup)");
    return -1;
  }

  struct stat source_st;
  if (lstat(src_path, &source_st) < 0) {
    if (errno == ENOENT) {
      return 0;
    }
    perror("lstat(check_src_against_backup)");
    return -1;
  }

  int src_is_dir = S_ISDIR(source_st.st_mode) && !S_ISLNK(source_st.st_mode);
  int bck_is_dir = S_ISDIR(backup_st.st_mode) && !S_ISLNK(backup_st.st_mode);

  if (src_is_dir != bck_is_dir ||
      (S_ISREG(source_st.st_mode) != S_ISREG(backup_st.st_mode)) ||
      (S_ISLNK(source_st.st_mode) != S_ISLNK(backup_st.st_mode))) {
    return rm_tree(src_path);
  }

  if (!src_is_dir)
    return 0;

  DIR *dir;
  if ((dir = opendir(src_path)) == NULL) {
    perror("opendir(check_src_against_backup)");
    return -1;
  }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;

    char src_child[PATH_MAX], bck_child[PATH_MAX];
    if (snprintf(src_child, PATH_MAX, "%s/%s", src_path, entry->d_name) >=
        PATH_MAX) {
      if (closedir(dir) < 0) {
        perror("closedir(check_src_against_backup)");
      }
      return -1;
    }
    if (snprintf(bck_child, PATH_MAX, "%s/%s", backup_path, entry->d_name) >=
        PATH_MAX) {
      if (closedir(dir) < 0) {
        perror("closedir(check_src_against_backup)");
      }
      return -1;
    }

    if (check_src_against_backup(src_child, bck_child) < 0) {
      if (closedir(dir) < 0) {
        perror("closedir(check_src_against_backup)");
      }
      return -1;
    }
  }

  if (closedir(dir) < 0) {
    perror("closedir(check_src_against_backup)");
    return -1;
  }
  return 0;
}

int apply_backup(const char *backup_path, const char *src_path,
                 const char *backup_real, const char *src_real,
                 time_t created_at) {
  struct stat backup_st;
  if (lstat(backup_path, &backup_st) < 0)
    return -1;

  if (S_ISDIR(backup_st.st_mode) && !S_ISLNK(backup_st.st_mode)) {
    if (mkdir_p(src_path, backup_st.st_mode & 0777) < 0)
      return -1;

    DIR *dir;
    if ((dir = opendir(backup_path)) == NULL) {
      perror("opendir(apply_backup)");
      return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
        continue;

      char bck_child[PATH_MAX], src_child[PATH_MAX];
      if (snprintf(bck_child, PATH_MAX, "%s/%s", backup_path, entry->d_name) >=
          PATH_MAX) {
        if (closedir(dir) < 0)
          perror("closedir(apply_backup)");
        return -1;
      }
      if (snprintf(src_child, PATH_MAX, "%s/%s", src_path, entry->d_name) >=
          PATH_MAX) {
        if (closedir(dir) < 0)
          perror("closedir(apply_backup)");
        return -1;
      }

      if (apply_backup(bck_child, src_child, backup_real, src_real,
                       created_at) < 0) {
        closedir(dir);
        return -1;
      }
    }
    if (closedir(dir) < 0) {
      perror("closedir(apply_backup)");
      return -1;
    }
    return 0;
  }

  struct stat source_st;
  int src_exists = (lstat(src_path, &source_st) == 0);
  int to_write = 0;
  if (!src_exists) {
    to_write = 1;
  } else if (source_st.st_mtime > created_at) {
    to_write = 1;
  }

  if (!to_write)
    return 0;

  // types dont match
  if (src_exists &&
      ((S_ISREG(source_st.st_mode) != S_ISREG(backup_st.st_mode)) ||
       (S_ISLNK(source_st.st_mode) != S_ISLNK(backup_st.st_mode)) ||
       (S_ISDIR(source_st.st_mode) &&
        !S_ISLNK(source_st.st_mode) != S_ISDIR(backup_st.st_mode) &&
        !S_ISLNK(backup_st.st_mode)))) {
    if (rm_tree(src_path) < 0)
      return -1;
  }

  if (ensure_parent_dir(src_path) < 0)
    return -1;

  if (S_ISREG(backup_st.st_mode)) {
    return copy_file(backup_path, src_path, backup_st.st_mode);
  }

  if (S_ISLNK(backup_st.st_mode)) {
    return copy_symplink_rewrite(backup_path, src_path, backup_real, src_real);
  }
  return 0;
}

// dynamic registry for backups
int ensure_capacity(BackupList *lst, size_t need) {
  if (lst->backups_capacity >= need) {
    return 0;
  }
  size_t new_capacity =
      (lst->backups_capacity == 0) ? 8 : (lst->backups_capacity * 2);
  while (new_capacity < need) {
    new_capacity *= 2;
  }
  Backup *new_backup = realloc(lst->backups, sizeof(Backup) * new_capacity);
  if (!new_backup) {
    return -1;
  }
  lst->backups = new_backup;
  lst->backups_capacity = new_capacity;
  return 0;
}

void free_backup(Backup *backup) {
  if (!backup) {
    return;
  }
  free(backup->dst);
  free(backup->src);
  backup->dst = NULL;
  backup->src = NULL;
  backup->created_at = 0;
  backup->active = 0;
}

int find_backup(char *src, char *dst) {
  for (int i = 0; i < (int)g_list.backups_count; i++) {
    if (strcmp(g_list.backups[i].src, src) == 0 &&
        strcmp(g_list.backups[i].dst, dst) == 0) {
      return i;
    }
  }
  return -1;
}

void reap_children() {
  while (1) {
    pid_t pid = waitpid(-1, NULL, WNOHANG);
    if (pid <= 0)
      break;

    for (size_t i = 0; i < g_list.backups_count; i++) {
      if (g_list.backups[i].active && g_list.backups[i].pid == pid) {
        g_list.backups[i].active = 0;
        g_list.backups[i].pid = 0;
        break;
      }
    }
  }
}

// helpers for copy to target
int is_dir_empty(char *path) {
  DIR *dir = opendir(path);
  if (!dir) {
    perror("opendir");
    return -1;
  }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    if (closedir(dir) < 0) {
      perror("closedir");
      return -1;
    }
    return 0; // not empty
  }
  if (closedir(dir) < 0) {
    perror("closedir");
    return -1;
  }
  return 1;
}

int mkdir_p(const char *path, mode_t mode) {
  char tmp[PATH_MAX];
  if (snprintf(tmp, sizeof(tmp), "%s", path) >= PATH_MAX) {
    printf("Path is too big\n");
    return -1;
  }

  size_t len = strlen(tmp);
  if (len == 0) {
    printf("Path is empty\n");
    return -1;
  }

  while (len > 1 && tmp[len - 1] == '/') {
    tmp[--len] = '\0';
  }

  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, mode) < 0 && errno != EEXIST) {
        perror("mkdir");
        return -1;
      }
      *p = '/';
    }
  }
  if (mkdir(tmp, mode) < 0 && errno != EEXIST) {
    perror("mkdir");
    return -1;
  }
  return 0;
}

int ensure_empty_dir(char *dst) {
  struct stat st;
  if (lstat(dst, &st) == 0) {
    if (!S_ISDIR(st.st_mode)) {
      fprintf(stderr, "%s is not a directory\n", dst);
      return -1;
    }

    int empty = is_dir_empty(dst);
    if (empty < 0) {
      perror("is_dir_empty");
      return -1;
    }
    if (empty == 0) {
      fprintf(stderr, "dst exists and is not empty\n");
      return -1;
    }
    return 0;
  }

  if (errno != ENOENT) {
    perror("lstat(dst)");
    return -1;
  }
  return 0;
}

int create_empty_dir(char *dst) {
  struct stat st;
  if (lstat(dst, &st) == 0) { // it already exists and 100% empty
    return 0;
  }

  if (mkdir_p(dst, 0755) < 0) {
    perror("mkdir_p(dst)");
    return -1;
  }
  return 0;
}

int has_prefix_path(const char *s, const char *prefix) {
  size_t len = strlen(prefix);
  if (strncmp(s, prefix, len) != 0) {
    return 0;
  }
  return (s[len] == '\0' || s[len] == '/');
}

int copy_file(const char *src, const char *dst, mode_t mode) {
  int in = open(src, O_RDONLY);
  if (in < 0) {
    perror("open src");
    return -1;
  }

  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
  if (out < 0) {
    perror("open dst");
    close(in);
    return -1;
  }

  char buf[1024];
  while (1) {
    if (g_child_exit) {
      if (close(in) < 0) {
        perror("close");
        return -1;
      }
      if (close(out) < 0) {
        perror("close");
        return -1;
      }
      errno = EINTR;
      return -1;
    }

    ssize_t r = bulk_read(in, buf, sizeof(buf));
    if (r < 0) {
      perror("bulk_read");
      if (close(in) < 0) {
        perror("close");
        return -1;
      }
      if (close(out) < 0) {
        perror("close");
        return -1;
      }
      return -1;
    }
    if (r == 0)
      break;

    ssize_t w = bulk_write(out, buf, r);
    if (w < 0) {
      perror("bulk_write");
      if (close(in) < 0) {
        perror("close");
      }
      if (close(out) < 0) {
        perror("close");
      }
      return -1;
    }
  }

  if (close(in) < 0) {
    perror("close");
    return -1;
  }
  if (close(out) < 0) {
    perror("close");
    return -1;
  }
  return 0;
}

int copy_symplink_rewrite(const char *src_link, const char *dst_link,
                          const char *src_real, const char *dst_real) {
  char linkbuf[PATH_MAX];
  ssize_t n = readlink(src_link, linkbuf, sizeof(linkbuf) - 1);
  if (n < 0) {
    perror("readlink");
    return -1;
  }
  linkbuf[n] = '\0';

  char *final_target = linkbuf;
  char rewritten[PATH_MAX];

  if (linkbuf[0] == '/' &&
      has_prefix_path(linkbuf, src_real)) { // checking if contains an absolute
                                            // path leading to this directory
    char *suffix = linkbuf + strlen(src_real);
    if (snprintf(rewritten, PATH_MAX, "%s%s", dst_real, suffix) >= PATH_MAX) {
      perror("Name too long(link)");
      return -1;
    }
    final_target = rewritten;
  }

  unlink(dst_link);
  if (symlink(final_target, dst_link) < 0) {
    perror("symlink");
    return -1;
  }
  return 0;
}

int copy_tree(const char *src_dir, const char *dst_dir, const char *src_real,
              const char *dst_real) {
  DIR *d = opendir(src_dir);
  if (!d) {
    perror("opendir(src_dir)");
    return -1;
  }

  struct dirent *entity;
  while ((entity = readdir(d)) != NULL) {
    if (g_child_exit == 1) {
      if (closedir(d) < 0) {
        perror("closedir");
        return -1;
      }
      return -1;
    }
    if (strcmp(entity->d_name, ".") == 0 || strcmp(entity->d_name, "..") == 0) {
      continue;
    }

    char src_path[PATH_MAX], dst_path[PATH_MAX];
    if (snprintf(src_path, PATH_MAX, "%s/%s", src_dir, entity->d_name) >=
        PATH_MAX) {
      if (closedir(d) < 0) {
        perror("closedir");
        return -1;
      }
      perror("Name too long(src_dir + name)");
      return -1;
    }
    if (snprintf(dst_path, PATH_MAX, "%s/%s", dst_dir, entity->d_name) >=
        PATH_MAX) {
      if (closedir(d) < 0) {
        perror("closedir");
        return -1;
      }
      perror("Name too long(dst_dir + name)");
      return -1;
    }

    struct stat st;
    if (lstat(src_path, &st) < 0) {
      if (closedir(d) < 0) {
        perror("closedir");
        return -1;
      }
      return -1;
    }

    if (S_ISDIR(st.st_mode)) {
      if (mkdir(dst_path, st.st_mode & 0777) < 0 && errno != EEXIST) {
        if (closedir(d) < 0) {
          perror("closedir");
          return -1;
        }
        return -1;
      }
      if (copy_tree(src_path, dst_path, src_real, dst_real) < 0) {
        if (closedir(d) < 0) {
          perror("closedir");
          return -1;
        }
        return -1;
      }
    } else if (S_ISREG(st.st_mode)) {
      if (copy_file(src_path, dst_path, st.st_mode) < 0) {
        if (closedir(d) < 0) {
          perror("closedir");
          return -1;
        }
        return -1;
      }
    } else if (S_ISLNK(st.st_mode)) {
      if (copy_symplink_rewrite(src_path, dst_path, src_real, dst_real) < 0) {
        if (closedir(d) < 0) {
          perror("closedir");
          return -1;
        }
        return -1;
      }
    } else {
      fprintf(stderr, "Skipping unsupported file type: %s\n", src_path);
    }
  }

  if (closedir(d) < 0) {
    perror("closedir");
    return -1;
  }
  return 0;
}

int rm_tree(const char *path) {
  struct stat st;
  if (lstat(path, &st) < 0) {
    if (errno == ENOENT)
      return 0;
    perror("lstat(rm_tree)");
    return -1;
  }

  if (S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path);
    if (!d) {
      perror("opendir(rm_tree)");
      return -1;
    }

    struct dirent *entity;
    while ((entity = readdir(d)) != NULL) {
      if (!strcmp(entity->d_name, ".") || !strcmp(entity->d_name, ".."))
        continue;

      char child[PATH_MAX];
      if (snprintf(child, PATH_MAX, "%s/%s", path, entity->d_name) >=
          PATH_MAX) {
        closedir(d);
        fprintf(stderr, "Name too long(rm_tree)\n");
        return -1;
      }
      if (rm_tree(child) < 0) {
        closedir(d);
        return -1;
      }
    }
    if (closedir(d) < 0) {
      perror("closedir(rm_tree)");
      return -1;
    }

    if (rmdir(path) < 0 && errno != ENOENT) {
      perror("rmdir(rm_tree)");
      return -1;
    }
    return 0;
  }

  if (unlink(path) < 0 && errno != ENOENT) {
    perror("unlink(rm_tree)");
    return -1;
  }
  return 0;
}

void child_loop(char *src, char *dst) {
  child_install_signals();

  char src_real[PATH_MAX];

  if (norm_existing_dir(src, src_real) < 0) {
    _exit(0);
  }

  if (create_empty_dir(dst)) {
    _exit(0);
  }

  char dst_real[PATH_MAX];
  if (!realpath(dst, dst_real)) {
    _exit(0);
  }

  copy_tree(src_real, dst_real, src_real, dst_real);

  if (monitor_and_mirror(src_real, dst_real) < 0)
    _exit(1);
  _exit(0);
}

// spawning
static int spawn_backup(char *src, char *dst) {
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return -1;
  }

  if (pid == 0) {
    child_loop(src, dst);
    _exit(EXIT_SUCCESS);
  }

  if (ensure_capacity(&g_list, g_list.backups_count + 1) < 0) {
    if (kill(pid, SIGTERM) < 0) {
      perror("kill");
    }
    return -1;
  }

  Backup new_backup;
  new_backup.src = strdup(src);
  new_backup.dst = strdup(dst);
  new_backup.pid = pid;
  new_backup.created_at = time(NULL);
  new_backup.active = 1;

  g_list.backups[g_list.backups_count++] = new_backup;
  return 0;
}

// commands
void cmd_help(void) {
  printf("Commands:\n");
  printf("  add <source> <target1> [target2 ...]\n");
  printf("  end <source> <target1> [target2 ...]\n");
  printf("  list\n");
  printf("  restore <source> <target>\n");
  printf("  exit\n");
}

void cmd_list() {
  reap_children();
  if (g_list.backups_count == 0) {
    printf("(no active backups)\n");
    return;
  }

  for (size_t i = 0; i < g_list.backups_count; i++) {
    if (g_list.backups[i].active) {
      printf("[ACTIVE] pid=%d src=\"%s\" dst=\"%s\"\n",
             (int)g_list.backups[i].pid, g_list.backups[i].src,
             g_list.backups[i].dst);
    } else {
      printf("[ENDED] src=\"%s\" dst=\"%s\"\n", g_list.backups[i].src,
             g_list.backups[i].dst);
    }
  }
}

void cmd_add(char *argv[], int argc) {
  if (argc < 3) {
    printf("usage: add <source> <target1> [target2 ...]\n");
    return;
  }

  char src_norm[PATH_MAX];
  if (norm_existing_dir(argv[1], src_norm) < 0) {
    printf("add: invalid source\n");
    return;
  }

  for (int i = 2; i < argc; i++) {
    char dst_norm[PATH_MAX];
    if (norm_target_path(argv[i], dst_norm) < 0) {
      printf("add: invalid target \"%s\"\n", argv[i]);
      continue;
    }

    if (has_prefix_path(dst_norm, src_norm)) {
      fprintf(stderr,
              "add: target is inside source (or same): src=\"%s\" dst=\"%s\"\n",
              src_norm, dst_norm);
      continue;
    }
    if (find_backup(src_norm, dst_norm) >= 0) {
      printf("add: already active src=\"%s\" dst=\"%s\"\n", src_norm, dst_norm);
      continue;
    }
    if (ensure_empty_dir(dst_norm) < 0) {
      perror("add: target invalid");
      continue;
    }
    if (spawn_backup(src_norm, dst_norm) >= 0) {
      printf("added src=\"%s\" -> dst=\"%s\"\n", src_norm, dst_norm);
    } else {
      printf("add failed for dst=\"%s\"\n", dst_norm);
    }
  }
}

void cmd_end(char *argv[], int argc) {
  if (argc < 3) {
    printf("usage: end <source> <target1> <target2> ...\n");
    return;
  }

  char src_norm[PATH_MAX];
  if (norm_existing_dir(argv[1], src_norm) < 0) {
    printf("add: invalid source\n");
    return;
  }

  for (int i = 2; i < argc; i++) {
    char dst_norm[PATH_MAX];
    if (norm_target_path(argv[i], dst_norm) < 0) {
      printf("add: invalid target \"%s\"\n", argv[i]);
      continue;
    }
    int index = find_backup(src_norm, dst_norm);
    if (index < 0) {
      printf("end: not found src=\"%s\" dst=\"%s\"\n", src_norm, dst_norm);
      continue;
    }

    if (!g_list.backups[index].active) {
      printf("end: already ended src=\"%s\" dst=\"%s\"\n",
             g_list.backups[index].src, g_list.backups[index].dst);
      continue;
    }

    if (kill(g_list.backups[index].pid, SIGTERM) < 0) {
      perror("kill");
    }
    if (waitpid(g_list.backups[index].pid, NULL, 0) < 0) {
      perror("waitpid");
    }
    g_list.backups[index].active = 0;
    g_list.backups[index].pid = 0;

    printf("ended src=\"%s\" dst=\"%s\" (backup kept for restore)\n",
           g_list.backups[index].src, g_list.backups[index].dst);
  }
}

void cmd_restore(char *argv[], int argc) {
  if (argc != 3) {
    printf("usage: end <source> <target1> <target2> ...\n");
    return;
  }

  char src_norm[PATH_MAX];
  if (norm_existing_dir(argv[1], src_norm) < 0) {
    printf("restore: invalid source\n");
    return;
  }

  char dst_norm[PATH_MAX];
  if (norm_target_path(argv[2], dst_norm) < 0) {
    printf("restore: invalid target \"%s\"\n", argv[2]);
    return;
  }

  int index = find_backup(src_norm, dst_norm);
  if (index < 0) {
    printf("restore: backup not found for this pair\n");
    return;
  }

  time_t created_at = g_list.backups[index].created_at;
  if (g_list.backups[index].active) {
    pid_t pid = g_list.backups[index].pid;
    if (kill(pid, SIGTERM) < 0) {
      perror("kill");
    }
    if (waitpid(pid, NULL, 0) < 0) {
      perror("waitpid");
    }
    g_list.backups[index].active = 0;
    g_list.backups[index].pid = 0;
  }

  if (check_src_against_backup(src_norm, dst_norm) < 0) {
    return;
  }
  if (apply_backup(dst_norm, src_norm, dst_norm, src_norm, created_at) < 0) {
    perror("apply backup");
    return;
  }

  printf("restored src=\"%s\" from backup=\"%s\"\n", src_norm, dst_norm);
}

int main() {
  install_parent_signals();
  cmd_help();

  char line[4096];
  while (!g_terminate) {
    reap_children();

    printf("> ");
    fflush(stdout);

    errno = 0;
    if (!fgets(line, sizeof(line), stdin)) {
      if (errno == EINTR) {
        printf("\n");
        continue;
      }
      break;
    }

    char *argv[MAX_ARGS];
    char args_buf[MAX_ARGS][PATH_MAX];
    int argc = 0;

    if (parse_line(line, argv, args_buf, &argc) < 0) {
      printf("parse error\n");
      continue;
    }
    if (argc == 0) {
      continue;
    }

    if (strcmp(argv[0], "help") == 0)
      cmd_help();
    else if (strcmp(argv[0], "list") == 0)
      cmd_list();
    else if (strcmp(argv[0], "add") == 0)
      cmd_add(argv, argc);
    else if (strcmp(argv[0], "end") == 0)
      cmd_end(argv, argc);
    else if (strcmp(argv[0], "restore") == 0)
      cmd_restore(argv, argc);
    else if (strcmp(argv[0], "exit") == 0)
      break;
    else
      printf("unknown command: %s\n", argv[0]);
  }

  for (size_t i = 0; i < g_list.backups_count; i++) {
    if (g_list.backups[i].active) {
      kill(g_list.backups[i].pid, SIGTERM);
    }
  }

  for (size_t i = 0; i < g_list.backups_count; i++) {
    if (g_list.backups[i].active) {
      if (waitpid(g_list.backups[i].pid, NULL, 0) < 0) {
        perror("waitpid");
      }
      g_list.backups[i].active = 0;
      g_list.backups[i].pid = 0;
    }
  }

  for (size_t i = 0; i < g_list.backups_count; i++)
    free_backup(&g_list.backups[i]);
  free(g_list.backups);

  return 0;
}