#ifndef IO_UTILS_H
#define IO_UTILS_H

#include <stddef.h>     // size_t
#include <sys/types.h>  // ssize_t

ssize_t bulk_read(int fd, char *buf, size_t count);
ssize_t bulk_write(int fd, char *buf, size_t count);

#endif
