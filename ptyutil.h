#if defined(__linux__) || defined(__CYGWIN__) || defined(__HAIKU__)
#include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__MACH__)
#include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <libutil.h>
#else

#ifdef __has_include

#if __has_include(<pty.h>)
#include <pty.h>
#elif __has_include(<util.h>)
#include <util.h>
#elif __has_include(<libutil.h>)
#include <libutil.h>
#endif

#endif

#endif
