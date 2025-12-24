#pragma once
#include <stdint.h>
#include <time.h>
#include "config.h"

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


struct WatchMap;

void pending_move_add(PendingMoves* pm, uint32_t cookie, int is_dir,
                      const char* src_old, const char* dst_old);

int pm_take(PendingMoves* pm, uint32_t cookie, PendingMove* out);

void pm_1s_expire(PendingMoves* pm, int notify_fd, struct WatchMap* map);