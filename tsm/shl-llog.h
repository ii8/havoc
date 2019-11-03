
#ifndef SHL_LLOG_H
#define SHL_LLOG_H

#include <stdio.h>

#define llog_debug(obj, fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#define llog_warning(obj, fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)

#endif
