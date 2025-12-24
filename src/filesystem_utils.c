#define _GNU_SOURCE
#include "filesystem_utils.h"
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

#include "io_utils.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

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

int copy_file(const char *src, const char *dst, mode_t mode, volatile sig_atomic_t* g_child_exit) {
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
    if (*g_child_exit) {
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
              const char *dst_real, volatile sig_atomic_t* g_child_exit) {
  DIR *d = opendir(src_dir);
  if (!d) {
    perror("opendir(src_dir)");
    return -1;
  }

  struct dirent *entity;
  while ((entity = readdir(d)) != NULL) {
    if (*g_child_exit == 1) {
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
      if (copy_tree(src_path, dst_path, src_real, dst_real, g_child_exit) < 0) {
        if (closedir(d) < 0) {
          perror("closedir");
          return -1;
        }
        return -1;
      }
    } else if (S_ISREG(st.st_mode)) {
      if (copy_file(src_path, dst_path, st.st_mode, g_child_exit) < 0) {
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