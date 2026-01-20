#ifndef RESTORE_H
#define RESTORE_H

#include <signal.h>
#include <time.h>

#include "config.h"

int check_src_against_backup(const char *src_path, const char *backup_path);

int apply_backup(const char *backup_path, const char *src_path,
                 const char *backup_real, const char *src_real,
                 time_t created_at, volatile sig_atomic_t *stop_flag);

#endif
