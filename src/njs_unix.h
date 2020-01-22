
/*
 * Copyright (C) Dmitry Volyntsev
 * Copyright (C) NGINX, Inc.
 */


#ifndef _NJS_UNIX_H_INCLUDED_
#define _NJS_UNIX_H_INCLUDED_

#if !defined(_MSC_VER)
#define njs_pagesize()      getpagesize()
#else
#define njs_pagesize()      (1024)
#endif

#if (NJS_LINUX)

#ifdef _FORTIFY_SOURCE
/*
 * _FORTIFY_SOURCE
 *     does not allow to use "(void) write()";
 */
#undef _FORTIFY_SOURCE
#endif

#endif /* NJS_LINUX */


#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <time.h>
#include <fcntl.h>

/*
 * alloca() is defined in stdlib.h in Linux, FreeBSD and MacOSX
 * and in alloca.h in Linux, Solaris and MacOSX.
 */
#if (NJS_SOLARIS)
#include <alloca.h>
#endif

#if !defined(_MSC_VER)
#include <sys/time.h>
#endif
#include <sys/stat.h>
#if !defined(_MSC_VER)
#include <sys/param.h>

#include <unistd.h>
#else
#include <stddef.h>
typedef int pid_t;
typedef unsigned int size_t;
typedef signed int ssize_t;
typedef unsigned char u_char;
#define __attribute__(x)
#define inline __inline
#if defined(_MSC_VER) && !defined(S_ISREG) && defined(_S_IFREG)
# define S_ISREG(x) (x & _S_IFREG)
#endif
#endif

#endif /* _NJS_UNIX_H_INCLUDED_ */
