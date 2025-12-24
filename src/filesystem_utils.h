#ifndef FILESYSTEM_UTILS_H
#define FILESYSTEM_UTILS_H

#include "config.h"     // for PATH_MAX 
#include <signal.h>     // sig_atomic_t
#include <sys/stat.h>   // mode_t
#include <sys/types.h>  // ssize_t

// Path normalization
int norm_existing_dir(const char *in, char out[PATH_MAX]);
void split_dir_base(char *path, char dir[PATH_MAX], char base[PATH_MAX]);
int norm_target_path(char *in, char out[PATH_MAX]);

// Directory helpers
int is_dir_empty(char *path);
int mkdir_p(const char *path, mode_t mode);
int ensure_empty_dir(char *dst);
int create_empty_dir(char *dst);

// Path prefix helper
int has_prefix_path(const char *s, const char *prefix);

// File / symlink / tree operations
int copy_file(const char *src, const char *dst, mode_t mode,
              volatile sig_atomic_t *stop_flag);

int copy_symplink_rewrite(const char *src_link, const char *dst_link,
                          const char *src_real, const char *dst_real);

int copy_tree(const char *src_dir, const char *dst_dir,
              const char *src_real, const char *dst_real,
              volatile sig_atomic_t *stop_flag);


int rm_tree(const char *path);

#endif
