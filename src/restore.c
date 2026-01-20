#define _GNU_SOURCE
#include "restore.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "filesystem_utils.h"
#include "mirror.h"

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
                 time_t created_at, volatile sig_atomic_t *stop_flag) {
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

      if (apply_backup(bck_child, src_child, backup_real, src_real, created_at,
                       stop_flag) < 0) {
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

  if (src_exists) {
    int src_is_dir = S_ISDIR(source_st.st_mode) && !S_ISLNK(source_st.st_mode);
    int bck_is_dir = S_ISDIR(backup_st.st_mode) && !S_ISLNK(backup_st.st_mode);
    int src_is_reg = S_ISREG(source_st.st_mode);
    int bck_is_reg = S_ISREG(backup_st.st_mode);
    int src_is_lnk = S_ISLNK(source_st.st_mode);
    int bck_is_lnk = S_ISLNK(backup_st.st_mode);
    if ((src_is_dir != bck_is_dir) || (src_is_reg != bck_is_reg) ||
        (src_is_lnk != bck_is_lnk)) {
      if (rm_tree(src_path) < 0)
        return -1;
    }
  }

  if (ensure_parent_dir(src_path) < 0)
    return -1;

  if (S_ISREG(backup_st.st_mode)) {
    return copy_file(backup_path, src_path, backup_st.st_mode, stop_flag);
  }

  if (S_ISLNK(backup_st.st_mode)) {
    return copy_symplink_rewrite(backup_path, src_path, backup_real, src_real);
  }
  return 0;
}
