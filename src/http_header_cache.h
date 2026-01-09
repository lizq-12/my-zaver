/*
 * Simple freelist cache for zv_http_header_t
 */

#ifndef ZV_HTTP_HEADER_CACHE_H
#define ZV_HTTP_HEADER_CACHE_H

#include "http_request.h"

zv_http_header_t *zv_http_header_alloc(void);
void zv_http_header_free(zv_http_header_t *hd);

#endif
