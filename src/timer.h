
/*
 * Copyright (C) Zhu Jiashun
 * Copyright (C) Zaver
 */

#ifndef ZV_TIMER_H
#define ZV_TIMER_H

#include "priority_queue.h"
#include "http_request.h"

#define ZV_TIMER_INFINITE -1
#define TIMEOUT_DEFAULT 5000     /* ms */

typedef int (*timer_handler_pt)(zv_http_request_t *rq);

typedef struct zv_timer_node_s{
    size_t key;
    int deleted;    //标记是否被删除 为1表示被删除
    timer_handler_pt handler;
    zv_http_request_t *rq;
} zv_timer_node;

int zv_timer_init();
int zv_find_timer();
void zv_handle_expire_timers();

extern zv_pq_t zv_timer;
extern size_t zv_current_msec;

void zv_add_timer(zv_http_request_t *rq, size_t timeout, timer_handler_pt handler);
void zv_del_timer(zv_http_request_t *rq);

#endif
