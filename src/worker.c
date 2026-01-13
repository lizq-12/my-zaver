/*
 * Zaver worker process (epoll loop)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "worker.h"
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#ifdef __linux__
#include <sched.h>
#endif
#include "dbg.h"
#include "epoll.h"
#include "http.h"
#include "cgi.h"
#include "http_request_cache.h"
#include "timer.h"
#include "ep_item.h"
#include <unistd.h>
#include <fcntl.h>
#include "zv_signal.h"

extern struct epoll_event *events;
// 判断是否为预期的断开连接错误码
static int is_expected_disconnect_errno(int e) {
    return (e == EPIPE || e == ECONNRESET);
}

// 忽略SIGPIPE信号，防止写入已关闭的socket导致进程终止
static int ignore_sigpipe(void) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    return sigaction(SIGPIPE, &sa, NULL);
}

static void maybe_set_cpu_affinity(const zv_conf_t *cf, int worker_id) {
    if (!cf || cf->cpu_affinity == 0) {
        return;
    }
#ifdef __linux__
    cpu_set_t available;//  CPU 位图集合类型
    CPU_ZERO(&available);// 初始化CPU集合，将所有位清0
    // 获取当前进程的CPU亲和性掩码，表示允许进程运行的CPU集合
    if (sched_getaffinity(0, sizeof(available), &available) != 0) {
        log_warn("sched_getaffinity failed");
        return;
    }

    int cpu_count = 0;
    int max_cpu = 0;
#ifdef CPU_SETSIZE
    max_cpu = CPU_SETSIZE;
#else
    max_cpu = (int)(8 * sizeof(cpu_set_t));
#endif
    // 计算可用CPU的数量
    for (int cpu = 0; cpu < max_cpu; cpu++) {
        if (CPU_ISSET(cpu, &available)) {
            cpu_count++;
        }
    }
    // 没有可用CPU
    if (cpu_count <= 0) {
        log_warn("no available CPU for affinity");
        return;
    }
    // 选择第几个可以用的CPU作为当前进程绑定的CPU
    int pick = worker_id % cpu_count;
    int chosen_cpu = -1;
    for (int cpu = 0; cpu < max_cpu; cpu++) {
        if (!CPU_ISSET(cpu, &available)) continue;
        if (pick == 0) {
            chosen_cpu = cpu;
            break;
        }
        pick--;
    }
    // 未选择到CPU
    if (chosen_cpu < 0) {
        return;
    }
    // 设置当前进程的CPU亲和性掩码
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(chosen_cpu, &set);// 将选中的CPU加入集合
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        log_warn("sched_setaffinity failed (cpu=%d)", chosen_cpu);
        return;
    }
    log_info("worker affinity set. worker_id=%d cpu=%d", worker_id, chosen_cpu);
#else
    (void)worker_id;
    log_warn("cpu_affinity enabled but not supported on this platform");
#endif
}

// worker进程的主循环
int zv_worker_run(zv_conf_t *cf, int worker_id) {
    int rc;
    // 安装SIGPIPE信号忽略处理函数
    if (ignore_sigpipe() != 0) {
        log_err("install sigal handler for SIGPIPE failed");
        return 1;
    }
    // 覆盖 master 进程的信号处理：保证 Ctrl+C / kill 能让 worker 退出
    if (zv_install_worker_signals() != 0) {
        log_err("install worker signals failed");
        return 1;
    }
    // 设置 CPU 亲和性
    maybe_set_cpu_affinity(cf, worker_id);

    //// 打开一个监听port的套接字，启用SO_REUSEPORT选项（用于多进程工作者）。
    // 打开监听套接字 每个worker进程独立监听同一端口
    int listenfd = open_listenfd_reuseport(cf->port);
    if (listenfd < 0) {
        log_err("open_listenfd_reuseport failed (port=%d)", cf->port);
        return 1;
    }
    // 将监听套接字设置为非阻塞模式
    rc = make_socket_non_blocking(listenfd);
    check(rc == 0, "make_socket_non_blocking(listenfd)");

    // 创建epoll实例和分配接收事件的数组
    int epfd = zv_epoll_create(0);
    struct epoll_event event;
    // 初始化监听事件数据结构
    zv_http_request_t *request = zv_http_request_get(listenfd, epfd, cf);
    check(request != NULL, "zv_http_request_get(listenfd)");
    // 将监听套接字添加到epoll实例中
    check(request->conn_item != NULL, "listen conn_item alloc failed");
    ((zv_ep_item_t *)request->conn_item)->kind = ZV_EP_KIND_LISTEN; // 事件类型为监听事件
    ((zv_ep_item_t *)request->conn_item)->fd = listenfd;
    ((zv_ep_item_t *)request->conn_item)->r = request;
    event.data.ptr = (void *)request->conn_item;
    event.events = EPOLLIN | EPOLLET;
    zv_epoll_add(epfd, listenfd, &event);

    // 初始化定时器模块
    zv_timer_init();
    log_info("zaver worker started. worker_id=%d pid=%d", worker_id, getpid());

    int n;
    int i, fd;
    int time;
    //初始化接收客户端信息结构体
    struct sockaddr_in clientaddr;
    socklen_t inlen = sizeof(struct sockaddr_in);
    memset(&clientaddr, 0, sizeof(struct sockaddr_in));
    // 进入主循环
    while (!zv_stop) 
    {
        time = zv_find_timer();// 获取最近的定时器超时时间
        n = zv_epoll_wait(epfd, events, MAXEVENTS, time);//用最近的定时器超时时间作为epoll_wait的超时时间
        // 处理就绪事件
        for (i = 0; i < n; i++) 
        {
            zv_ep_item_t *it = (zv_ep_item_t *)events[i].data.ptr;
            if (!it) continue;

            zv_http_request_t *r = it->r;
            fd = it->fd;

            if (it->kind == ZV_EP_KIND_LISTEN) {
                int infd;
                while (1) {//循环接受所有到来的连接
                    infd = accept(listenfd, (struct sockaddr *)&clientaddr, &inlen);
                    if (infd < 0) {
                        //因为是非阻塞accept 所以没有连接时会返回EAGAIN或EWOULDBLOCK错误码
                        if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                            break;
                        } else {
                            log_err("accept");
                            break;
                        }
                    }
                    // 设置新连接套接字为非阻塞模式
                    rc = make_socket_non_blocking(infd);
                    check(rc == 0, "make_socket_non_blocking(infd)");

                    // 禁用 Nagle 算法，减少延迟
                    int one = 1;
                    if (setsockopt(infd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
                        log_warn("setsockopt TCP_NODELAY failed, fd=%d", infd);
                    }
                    // 为新连接分配请求结构体
                    zv_http_request_t *req = zv_http_request_get(infd, epfd, cf);
                    if (req == NULL) {
                        log_err("zv_http_request_get(infd)");
                        close(infd);
                        break;
                    }
                    if (!req->conn_item) {
                        log_err("conn_item alloc failed");
                        close(infd);
                        zv_http_request_put_deferred(req);
                        break;
                    }
                    // 将新连接套接字添加到epoll实例中，监听读事件
                    // EPOLLONESHOT表示事件触发后需要重新注册才能继续监听该事件
                    event.data.ptr = (void *)req->conn_item;
                    event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
                    zv_epoll_add(epfd, infd, &event);
                    zv_add_timer(req, req->keep_alive_timeout_ms, zv_http_close_conn);// idle timeout
                }
            } else if (it->kind == ZV_EP_KIND_CGI_OUT) {
                if (!r) continue;
                /* CGI stdout is readable (or closed/error) */
                zv_cgi_on_stdout_ready(r);
                continue;
            } else if (it->kind == ZV_EP_KIND_CGI_IN) {
                /* GET-only MVP: not used */
                continue;
            } else // 处理已连接套接字的事件
            {
                uint32_t ev = events[i].events;
                // 处理错误事件
                if (ev & EPOLLERR) {
                    int so_error = 0;
                    socklen_t so_error_len = sizeof(so_error);
                    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) == 0 && so_error != 0) {
                        errno = so_error;
                    } else {
                        errno = 0;
                    }

                    if (errno == 0 || is_expected_disconnect_errno(errno)) {
                        debug("epoll peer disconnect fd: %d, events=0x%x, errno=%d", r->fd, ev, errno);
                    } else {
                        log_err("epoll event error fd: %d, events=0x%x", r->fd, ev);
                    }
                    zv_del_timer(r);
                    zv_http_close_conn(r);
                    continue;
                }
                // 处理读事件
                if (ev & EPOLLIN) {
                    if (r) do_request(r);
                    continue;
                }
                // 处理写事件
                if (ev & EPOLLOUT) {
                    if (r) do_write(r);
                    continue;
                }
                // 处理挂起或半关闭事件
                if (ev & (EPOLLHUP | EPOLLRDHUP)) {
                    errno = 0;
                    log_err("epoll hangup fd: %d, events=0x%x", r->fd, ev);
                    zv_del_timer(r);
                    zv_http_close_conn(r);
                    continue;
                }
                continue;
            }
        }
        // 处理到期定时器
        zv_handle_expire_timers();

        /* Safe point: now it is ok to return closed requests to freelist. */
        zv_http_request_deferred_flush();
    }

    // best-effort cleanup
    if (request) {
        zv_http_request_put(request);
        request = NULL;
    }

    zv_http_request_cache_dump_stats();
    close(listenfd);
    close(epfd);
    if (events) {
        free(events);
        events = NULL;
    }

    return 0;
}
