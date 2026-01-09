
/*
 * Copyright (C) Zhu Jiashun
 * Copyright (C) Zaver
 */

#include "epoll.h"
#include "dbg.h"

struct epoll_event *events;

int zv_epoll_create(int flags) {
    int fd = epoll_create1(flags);
    check(fd > 0, "zv_epoll_create: epoll_create1");

    events = (struct epoll_event *)malloc(sizeof(struct epoll_event) * MAXEVENTS);
    check(events != NULL, "zv_epoll_create: malloc");
    return fd;
}
// 添加监听事件
void zv_epoll_add(int epfd, int fd, struct epoll_event *event) {
    int rc = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, event);
    check(rc == 0, "zv_epoll_add: epoll_ctl");
    return;
}
// 修改监听事件
void zv_epoll_mod(int epfd, int fd, struct epoll_event *event) {
    int rc = epoll_ctl(epfd, EPOLL_CTL_MOD, fd, event);
    check(rc == 0, "zv_epoll_mod: epoll_ctl");
    return;
}
// 删除监听事件
void zv_epoll_del(int epfd, int fd, struct epoll_event *event) {
    int rc = epoll_ctl(epfd, EPOLL_CTL_DEL, fd, event);
    check(rc == 0, "zv_epoll_del: epoll_ctl");
    return;
}
// 等待事件发生
int zv_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
    int n = epoll_wait(epfd, events, maxevents, timeout);
    if (n < 0) {
        // 被信号中断的情况下，直接返回0
        if (errno == EINTR) {
            return 0;
        }
        //其他错误情况
        check(0, "zv_epoll_wait: epoll_wait");
        return -1;
    }

    return n;
}
