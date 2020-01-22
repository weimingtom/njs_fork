
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NJS_MALLOC_H_INCLUDED_
#define _NJS_MALLOC_H_INCLUDED_


#define njs_malloc(size)   malloc(size)
#if !defined(_MSC_VER)
#define njs_free(p)        free(p)
#else
#define njs_free(p)        _aligned_free(p)
#endif

NJS_EXPORT void *njs_zalloc(size_t size) NJS_MALLOC_LIKE;
NJS_EXPORT void *njs_memalign(size_t alignment, size_t size) NJS_MALLOC_LIKE;


#endif /* _NJS_MALLOC_H_INCLUDED_ */
