#ifndef MONITOR_H
#define MONITOR_H

#include <signal.h>  //sig_atomic_t


int monitor_and_mirror(const char *src_real, const char *dst_real,
                       volatile sig_atomic_t *stop_flag);

#endif 
