/*
 * Simple freelist cache for zv_http_request_t
 */

#ifndef ZV_HTTP_REQUEST_CACHE_H
#define ZV_HTTP_REQUEST_CACHE_H

#include "http_request.h"

zv_http_request_t *zv_http_request_get(int fd, int epfd, zv_conf_t *cf);
void zv_http_request_put(zv_http_request_t *r);
/* Defer putting requests back into the freelist until a safe point (end of epoll batch). */
void zv_http_request_put_deferred(zv_http_request_t *r);
void zv_http_request_deferred_flush(void);
/* Print cache stats once (process-local). */
void zv_http_request_cache_dump_stats(void);

#endif
