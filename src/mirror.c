#define _GNU_SOURCE
#include "mirror.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "filesystem_utils.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

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
                            const char *src_real, const char *dst_real, volatile sig_atomic_t *stop_flag) {
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
    return copy_file(src_path, dst_path, st.st_mode, stop_flag);
  }
  if (S_ISLNK(st.st_mode)) {
    return copy_symplink_rewrite(src_path, dst_path, src_real, dst_real);
  }

  return 0;
}

int mirror_delete_path(char *dst_path) { return rm_tree(dst_path); }