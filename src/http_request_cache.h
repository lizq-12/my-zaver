/*
 * Simple freelist cache for zv_http_request_t
 */

#ifndef ZV_HTTP_REQUEST_CACHE_H
#define ZV_HTTP_REQUEST_CACHE_H

#include "http_request.h"

zv_http_request_t *zv_http_request_get(int fd, int epfd, zv_conf_t *cf);
void zv_http_request_put(zv_http_request_t *r);

/* Print cache stats once (process-local). */
void zv_http_request_cache_dump_stats(void);

#endif
