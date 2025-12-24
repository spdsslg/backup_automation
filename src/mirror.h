#ifndef MIRROR_H
#define MIRROR_H

#include <signal.h>  //sig_atomic_t

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

int map_src_to_dst(const char *src_real, const char *dst_real,
                   const char *src_path, char dst_path[PATH_MAX]);

int ensure_parent_dir(const char *fullpath);

int mirror_create_or_update(const char *src_path, const char *dst_path,
                            const char *src_real, const char *dst_real,
                            volatile sig_atomic_t *stop_flag);

int mirror_delete_path(char *dst_path);

#endif
