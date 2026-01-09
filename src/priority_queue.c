
/*
 * Copyright (C) Zhu Jiashun
 * Copyright (C) Zaver
 */

#include "priority_queue.h"
//创建优先队列
int zv_pq_init(zv_pq_t *zv_pq, zv_pq_comparator_pt comp, size_t size) {
    zv_pq->pq = (void **)malloc(sizeof(void *) * (size+1));//分配 size+1 个指针空间，pq[0]不用
    if (!zv_pq->pq) {
        log_err("zv_pq_init: malloc failed");
        return -1;
    }
    
    zv_pq->nalloc = 0;
    zv_pq->size = size + 1;
    zv_pq->comp = comp;
    
    return ZV_OK;
}
//判断优先队列是否为空
int zv_pq_is_empty(zv_pq_t *zv_pq) {
    return (zv_pq->nalloc == 0)? 1: 0;
}
//获取优先队列大小
size_t zv_pq_size(zv_pq_t *zv_pq) {
    return zv_pq->nalloc;
}
//获取堆顶元素也就是最小元素
void *zv_pq_min(zv_pq_t *zv_pq) {
    if (zv_pq_is_empty(zv_pq)) {
        return NULL;
    }

    return zv_pq->pq[1];
}
//动态调整堆空间大小
static int resize(zv_pq_t *zv_pq, size_t new_size) {
    if (new_size <= zv_pq->nalloc) {
        log_err("resize: new_size to small");
        return -1;
    }

    void **new_ptr = (void **)malloc(sizeof(void *) * new_size);
    if (!new_ptr) {
        log_err("resize: malloc failed");
        return -1;
    }

    memcpy(new_ptr, zv_pq->pq, sizeof(void *) * (zv_pq->nalloc + 1));
    free(zv_pq->pq);
    zv_pq->pq = new_ptr;
    zv_pq->size = new_size;
    return ZV_OK;
}
//交换位置i和j的元素
static void exch(zv_pq_t *zv_pq, size_t i, size_t j) {
    void *tmp = zv_pq->pq[i];
    zv_pq->pq[i] = zv_pq->pq[j];
    zv_pq->pq[j] = tmp;
}
//上浮k位置的元素
static void swim(zv_pq_t *zv_pq, size_t k) {
    while (k > 1 && zv_pq->comp(zv_pq->pq[k], zv_pq->pq[k/2])) {
        exch(zv_pq, k, k/2);
        k /= 2;
    }
}
//下沉k位置的元素 返回最终位置
static size_t sink(zv_pq_t *zv_pq, size_t k) {
    size_t j;
    size_t nalloc = zv_pq->nalloc;

    while (2*k <= nalloc) {
        j = 2*k;
        if (j < nalloc && zv_pq->comp(zv_pq->pq[j+1], zv_pq->pq[j])) j++;
        if (!zv_pq->comp(zv_pq->pq[j], zv_pq->pq[k])) break;
        exch(zv_pq, j, k);
        k = j;
    }
    
    return k;
}
//删除堆顶元素
int zv_pq_delmin(zv_pq_t *zv_pq) {
    if (zv_pq_is_empty(zv_pq)) {//空堆
        return ZV_OK;
    }
    //交换堆顶和堆尾元素
    exch(zv_pq, 1, zv_pq->nalloc);
    zv_pq->nalloc--;//堆大小减1
    sink(zv_pq, 1);//下沉新的堆顶元素
    //动态缩小堆空间
    if (zv_pq->nalloc > 0 && zv_pq->nalloc <= (zv_pq->size - 1)/4) {
        if (resize(zv_pq, zv_pq->size / 2) < 0) {
            return -1;
        }
    }

    return ZV_OK;
}
//插入元素并上浮
int zv_pq_insert(zv_pq_t *zv_pq, void *item) {
    //动态扩大堆空间
    if (zv_pq->nalloc + 1 == zv_pq->size) {
        if (resize(zv_pq, zv_pq->size * 2) < 0) {
            return -1;
        }
    }
    //插入新元素并上浮
    zv_pq->pq[++zv_pq->nalloc] = item;
    swim(zv_pq, zv_pq->nalloc);

    return ZV_OK;
}
//对位置i的元素下沉
int zv_pq_sink(zv_pq_t *zv_pq, size_t i) {
    return sink(zv_pq, i);
}
