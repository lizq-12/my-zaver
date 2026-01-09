/*
 * Simple freelist cache for zv_http_request_t
 */

#include "http_request_cache.h"

#include <stdlib.h>
#include <string.h>

#include "dbg.h"
#include "list.h"

#ifndef ZV_REQUEST_FREELIST_MAX
#define ZV_REQUEST_FREELIST_MAX 65536
#endif

/* We reuse zv_http_request_t memory blocks across connections within a worker process.
 * This is intentionally process-local (no locks).
 */
static list_head g_free_requests;
static size_t g_free_count;
static int g_inited;

static size_t g_get_calls;
static size_t g_get_hits;
static size_t g_get_mallocs;
static size_t g_put_calls;
static size_t g_put_frees;
static size_t g_max_free_count;

static void init_once(void) {
    if (!g_inited) {
        INIT_LIST_HEAD(&g_free_requests);
        g_free_count = 0;
        g_inited = 1;

        g_get_calls = 0;
        g_get_hits = 0;
        g_get_mallocs = 0;
        g_put_calls = 0;
        g_put_frees = 0;
        g_max_free_count = 0;
    }
}
//获取一个 zv_http_request_t 结构体实例
zv_http_request_t *zv_http_request_get(int fd, int epfd, zv_conf_t *cf) {
    init_once();

    g_get_calls++;

    zv_http_request_t *r = NULL;
    //如果缓存链表不为空则直接取出一个，否则新分配一个
    if (!list_empty(&g_free_requests)) {
        list_head *pos = g_free_requests.next;
        list_del(pos);
        r = list_entry(pos, zv_http_request_t, freelist);
        g_get_hits++;
        if (g_free_count > 0) {
            g_free_count--;
        }
    } else {
        r = (zv_http_request_t *)malloc(sizeof(zv_http_request_t));
        if (!r) return NULL;
        g_get_mallocs++;
    }
    //初始化请求结构体
    (void)zv_init_request_t(r, fd, epfd, cf);
    
    return r;
}
//释放 zv_http_request_t 结构体到缓存链表
void zv_http_request_put(zv_http_request_t *r) {
    if (!r) return;

    init_once();

    g_put_calls++;

    if (g_free_count >= ZV_REQUEST_FREELIST_MAX) {
        free(r);
        g_put_frees++;
        return;
    }

    INIT_LIST_HEAD(&r->freelist);
    list_add(&r->freelist, &g_free_requests);
    g_free_count++;

    if (g_free_count > g_max_free_count) {
        g_max_free_count = g_free_count;
    }
}

void zv_http_request_cache_dump_stats(void) {
    if (!g_inited) {
        return;
    }

    log_status("request_cache: get=%zu hit=%zu malloc=%zu put=%zu free=%zu free_now=%zu free_max=%zu max_cap=%d",
             g_get_calls,
             g_get_hits,
             g_get_mallocs,
             g_put_calls,
             g_put_frees,
             g_free_count,
             g_max_free_count,
             (int)ZV_REQUEST_FREELIST_MAX);
}
