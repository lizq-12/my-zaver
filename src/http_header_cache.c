/*
 * Simple freelist cache for zv_http_header_t
 */

#include "http_header_cache.h"
#include <stdlib.h>
#include <string.h>
#include "list.h"
//定义缓存的最大数量
#ifndef ZV_HEADER_FREELIST_MAX
#define ZV_HEADER_FREELIST_MAX 8192
#endif

static list_head g_free_headers;
static size_t g_free_count;
static int g_inited;

//初始化缓存链表
static void init_once(void) {
    if (!g_inited) {
        INIT_LIST_HEAD(&g_free_headers);//初始化链表头
        g_free_count = 0;//初始化缓存数量
        g_inited = 1;//标记已初始化
    }
}
//分配或从缓存链表获取一个 http header 结构体
zv_http_header_t *zv_http_header_alloc(void) {
    init_once();

    zv_http_header_t *hd = NULL;
    //如果空闲缓存链表不为空 则从链表头取出一个节点
    if (!list_empty(&g_free_headers)) {
        list_head *pos = g_free_headers.next;
        list_del(pos);
        hd = list_entry(pos, zv_http_header_t, list);
        if (g_free_count > 0) {
            g_free_count--;
        }
    } 
    else {//否则分配新的节点
        hd = (zv_http_header_t *)malloc(sizeof(zv_http_header_t));
        if (!hd) {
            return NULL;
        }
    }

    memset(hd, 0, sizeof(*hd));
    INIT_LIST_HEAD(&hd->list);
    return hd;
}
//释放 http header结构体到缓存链表
void zv_http_header_free(zv_http_header_t *hd) {
    if (!hd) return;

    init_once();

    if (g_free_count >= ZV_HEADER_FREELIST_MAX) {
        free(hd);
        return;
    }

    hd->key_start = hd->key_end = NULL;
    hd->value_start = hd->value_end = NULL;
    INIT_LIST_HEAD(&hd->list);//清空链表指针
    list_add(&hd->list, &g_free_headers);//加入到缓存链表头部
    g_free_count++;//增加缓存数量
}
