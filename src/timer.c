
/*
 * Copyright (C) Zhu Jiashun
 * Copyright (C) Zaver
 */

#include <time.h>
#include "timer.h"
//比较函数，key返回1表示ti在tj之前，即ti优先级更高
static int timer_comp(void *ti, void *tj) {
    zv_timer_node *timeri = (zv_timer_node *)ti;
    zv_timer_node *timerj = (zv_timer_node *)tj;

    return (timeri->key < timerj->key)? 1: 0;
}

zv_pq_t zv_timer;
size_t zv_current_msec;

//更新当前时间 保存在一个全局时间变量上
static void zv_time_update() {
    // there is only one thread calling zv_time_update, no need to lock?
    struct timespec ts;
    int rc;

    rc = clock_gettime(CLOCK_MONOTONIC, &ts);
    check(rc == 0, "zv_time_update: clock_gettime error");

    zv_current_msec = (size_t)ts.tv_sec * 1000 + (size_t)ts.tv_nsec / 1000000; // 转换成毫秒
    debug("in zv_time_update, time = %zu", zv_current_msec);
}

//初始化优先队列
int zv_timer_init() {
    int rc;
    rc = zv_pq_init(&zv_timer, timer_comp, ZV_PQ_DEFAULT_SIZE);
    check(rc == ZV_OK, "zv_pq_init error");
   
    zv_time_update();
    return ZV_OK;
}
//从堆顶开始找到第一个未被删除的定时器节点 计算时间差返回
//会顺便更新当前时间 清理过期定时器
int zv_find_timer() {
    zv_timer_node *timer_node;
    int time = ZV_TIMER_INFINITE;//默认值-1 表示阻塞
    int rc;
    
    while (!zv_pq_is_empty(&zv_timer)) {//不为空时
        debug("zv_find_timer");
        zv_time_update();
        timer_node = (zv_timer_node *)zv_pq_min(&zv_timer);//获取堆顶元素
        check(timer_node != NULL, "zv_pq_min error");
        //如果被标记为删除 则删除堆顶元素并释放内存 继续循环
        if (timer_node->deleted) {
            rc = zv_pq_delmin(&zv_timer); 
            check(rc == 0, "zv_pq_delmin");
            free(timer_node);
            continue;
        }
        //否则计算时间差返回     
        time = (int) (timer_node->key - zv_current_msec);
        debug("in zv_find_timer, key = %zu, cur = %zu",
                timer_node->key,
                zv_current_msec);
        time = (time > 0? time: 0);//如果时间差小于0 则返回0
        break;
    }
    
    return time;
}
//处理到期定时器 执行回调函数(关闭连接) 并删除定时器节点 
//一直会处理到未到期的定时器
void zv_handle_expire_timers() {
    debug("in zv_handle_expire_timers");
    zv_timer_node *timer_node;
    int rc;

    while (!zv_pq_is_empty(&zv_timer)) {//不为空时
        debug("zv_handle_expire_timers, size = %zu", zv_pq_size(&zv_timer));
        zv_time_update();
        timer_node = (zv_timer_node *)zv_pq_min(&zv_timer);
        check(timer_node != NULL, "zv_pq_min error");
        //如果被标记为删除 则删除堆顶元素并释放内存 继续循环
        if (timer_node->deleted) {
            rc = zv_pq_delmin(&zv_timer); 
            check(rc == 0, "zv_handle_expire_timers: zv_pq_delmin error");
            free(timer_node);
            continue;
        }
        //如果未到期则直接返回
        if (timer_node->key > zv_current_msec) {
            return;
        }
        //到期则执行回调函数
        if (timer_node->handler) {
            if (timer_node->rq) {
                timer_node->rq->timer = NULL;
            }
            log_info("time out, closed fd %d", timer_node->rq->fd);
            timer_node->handler(timer_node->rq);
        }
        rc = zv_pq_delmin(&zv_timer);  // 删除堆顶元素并释放内存
        check(rc == 0, "zv_handle_expire_timers: zv_pq_delmin error");
        free(timer_node);
    }
}
//创建定时器节点并插入优先队列
//让定时器节点与http_request关联
void zv_add_timer(zv_http_request_t *rq, size_t timeout, timer_handler_pt handler) {
    int rc;
    zv_timer_node *timer_node = (zv_timer_node *)malloc(sizeof(zv_timer_node));
    check(timer_node != NULL, "zv_add_timer: malloc error");

    zv_time_update();
    rq->timer = timer_node;//rq关联定时器节点
    timer_node->key = zv_current_msec + timeout;
    debug("in zv_add_timer, key = %zu", timer_node->key);
    timer_node->deleted = 0;
    timer_node->handler = handler;
    timer_node->rq = rq;//定时器节点关联rq
    //插入优先队列
    rc = zv_pq_insert(&zv_timer, timer_node);
    check(rc == 0, "zv_add_timer: zv_pq_insert error");
}
//让http_request关联的定时器节点被标记为删除
void zv_del_timer(zv_http_request_t *rq) {
    debug("in zv_del_timer");
    zv_time_update();
    zv_timer_node *timer_node = rq->timer;
    check(timer_node != NULL, "zv_del_timer: rq->timer is NULL");

    timer_node->deleted = 1;
    rq->timer = NULL;
}
