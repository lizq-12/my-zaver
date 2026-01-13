#include "cgi.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/uio.h>
#include "dbg.h"
#include "epoll.h"
#include "timer.h"
#include "util.h"
#include "ep_item.h"

// 设置文件描述符为非阻塞且关闭时关闭（cloexec）
//FD_CLOEXEC标志 ：当进程调用 execve()（或其他 exec 族函数）替换为新程序时，带有该标志的文件描述符会被内核自动关闭；
static int set_nonblocking_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;

    int fdflags = fcntl(fd, F_GETFD, 0);
    if (fdflags >= 0) {
        (void)fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);
    }
    return 0;
}
// 解析 CGI 输出的状态码行 Status: xxx
static int parse_status_code(const char *line, int *status_out) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    // 查找 "Status:" 字段 如果不是则返回0
    if (strncasecmp(p, "Status:", 7) != 0) return 0;
    p += 7;
    while (*p == ' ' || *p == '\t') p++;
    // 解析状态码数字
    int code = atoi(p);
    // 验证状态码范围
    if (code >= 100 && code <= 599) {
        *status_out = code;
        return 1;
    }
    return 0;
}
// 解析 CGI 输出的内容类型 Content-Type: ...
static int parse_content_type(const char *line, char *out, size_t out_cap) {
    if (!out || out_cap == 0) return 0;
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    // 查找 "Content-Type:" 字段 如果不是则返回0
    if (strncasecmp(p, "Content-Type:", 13) != 0) return 0;
    p += 13;
    while (*p == ' ' || *p == '\t') p++;
    size_t n = strlen(p);
    // 去掉结尾的 \r\n
    while (n > 0 && (p[n - 1] == '\r' || p[n - 1] == '\n')) n--;
    if (n == 0) return 0;
    /* Don't silently truncate: treat as unparseable if it doesn't fit. */
    if (n >= out_cap) return 0;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}
// 构建 CGI 响应的 HTTP 头
/*"HTTP/1.1 %d %s\r\n"
"Server: Zaver\r\n"
"Connection: close\r\n"
"Content-Type: %s\r\n"
"\r\n"*/
static int build_http_header(zv_http_request_t *r, int status, const char *content_type) {
    if (!r) return -1;
    // 获取状态码对应的短语
    const char *reason = get_shortmsg_from_status_code(status);
    if (!reason || strcmp(reason, "Unknown") == 0) {
        reason = "OK";
        if (status >= 400 && status < 500) reason = "Bad Request";
        if (status >= 500) reason = "Internal Server Error";
    }
    //CGI 没给 Content-Type（或者解析失败）时，用默认的 text/plain。
    if (!content_type || content_type[0] == '\0') {
        content_type = "text/plain";
    }
    // 构建 HTTP 响应头
    int n = snprintf(r->cgi_http_header, sizeof(r->cgi_http_header),
                     "HTTP/1.1 %d %s\r\n"
                     "Server: Zaver\r\n"
                     "Connection: close\r\n"
                     "Content-Type: %s\r\n"
                     "\r\n",
                     status, reason, content_type);
    if (n < 0 || (size_t)n >= sizeof(r->cgi_http_header)) return -1;
    r->cgi_http_header_len = (size_t)n;
    r->cgi_http_header_sent = 0;
    return 0;
}
// 确保 CGI 输出事件的 epoll item 已分配并正确初始化
static void ensure_cgi_items(zv_http_request_t *r) {
    if (!r) return;
    if (r->cgi_out_item == NULL) {
        r->cgi_out_item = (struct zv_ep_item_s *)malloc(sizeof(struct zv_ep_item_s));
    }
    if (r->cgi_out_item) {
        zv_ep_item_t *it = (zv_ep_item_t *)r->cgi_out_item;
        it->kind = ZV_EP_KIND_CGI_OUT;
        it->fd = r->cgi_out_fd;
        it->r = r;
    }
}
// 启动 CGI 脚本运行
int zv_cgi_start(zv_http_request_t *r, const char *script_filename, const char *script_name, const char *query_string) {
    if (!r || !script_filename || !script_name) return -1;

    /*pipe 的两个 fd 含义（约定）：
    in_pipe[0]：读端（给子进程当 stdin）
    in_pipe[1]：写端（父进程写入子进程 stdin）
    out_pipe[0]：读端（父进程读取子进程 stdout）
    out_pipe[1]：写端（子进程输出到 stdout）
    MVP 里只支持 GET，所以不会给 CGI stdin 写请求体，但仍然建了 in_pipe，原因是代码结构上最通用：之后要支持 POST，直接复用即可。*/
    int in_pipe[2];
    int out_pipe[2];
    if (pipe(in_pipe) != 0) return -1;
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return -1;
    }
    //子进程：负责 dup2 重定向 stdin/stdout 并 execve 运行脚本
    //父进程（worker）：继续 epoll loop，不阻塞，负责读取 CGI 输出并写回 client
    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return -1;
    }
    /* child */
    if (pid == 0) {
        // 重定向 stdin/stdout
        (void)dup2(in_pipe[0], STDIN_FILENO);
        (void)dup2(out_pipe[1], STDOUT_FILENO);
        // 关闭不需要的 fd
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        // 设置 CGI 环境变量
        /*REQUEST_METHOD=GET
        QUERY_STRING=...
        SCRIPT_NAME=...
        SCRIPT_FILENAME=...
        GATEWAY_INTERFACE=CGI/1.1
        SERVER_PROTOCOL=HTTP/1.1
        SERVER_SOFTWARE=Zaver*/
        char method_buf[32];
        snprintf(method_buf, sizeof(method_buf), "REQUEST_METHOD=GET");
        char qs_buf[1024];
        if (query_string) {
            snprintf(qs_buf, sizeof(qs_buf), "QUERY_STRING=%s", query_string);
        } else {
            snprintf(qs_buf, sizeof(qs_buf), "QUERY_STRING=");
        }
        char script_name_buf[1024];
        snprintf(script_name_buf, sizeof(script_name_buf), "SCRIPT_NAME=%s", script_name);
        char script_file_buf[2048];
        snprintf(script_file_buf, sizeof(script_file_buf), "SCRIPT_FILENAME=%s", script_filename);
        char gw_buf[64];
        snprintf(gw_buf, sizeof(gw_buf), "GATEWAY_INTERFACE=CGI/1.1");
        char proto_buf[64];
        snprintf(proto_buf, sizeof(proto_buf), "SERVER_PROTOCOL=HTTP/1.1");
        char soft_buf[64];
        snprintf(soft_buf, sizeof(soft_buf), "SERVER_SOFTWARE=Zaver");

        char *envp[] = {
            gw_buf,
            proto_buf,
            soft_buf,
            method_buf,
            qs_buf,
            script_name_buf,
            script_file_buf,
            NULL
        };

        char *argv[] = {(char *)script_filename, NULL};
        execve(script_filename, argv, envp);
        _exit(127);
    }

    /* parent */
    close(in_pipe[0]);
    close(out_pipe[1]);
    // 把 stdout 读端设为 nonblocking，挂进 epoll
    if (set_nonblocking_cloexec(out_pipe[0]) != 0) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return -1;
    }
    /* stdin pipe not used for GET MVP; close it to signal EOF */
    //stdin 管道不用于 GET MVP；关闭它以发出 EOF 信号
    close(in_pipe[1]);
    //初始化 request 里的 CGI 状态字段
    r->cgi_active = 1;
    r->cgi_pid = pid;
    r->cgi_in_fd = -1;
    r->cgi_out_fd = out_pipe[0];
    r->cgi_eof = 0;
    r->cgi_out_total = 0;
    r->cgi_headers_done = 0;
    r->cgi_hdr_len = 0;
    r->cgi_http_header_len = 0;
    r->cgi_http_header_sent = 0;
    r->cgi_body_len = 0;
    r->cgi_body_sent = 0;
    //准备 epoll data.ptr 的 “item” 结构
    ensure_cgi_items(r);
    //把 CGI stdout fd 加入 epoll 监听读事件
    struct epoll_event ev;
    ev.data.ptr = (void *)r->cgi_out_item;
    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    zv_epoll_add(r->epfd, r->cgi_out_fd, &ev);

    /* CGI responses are streamed without Content-Length; force close. */
    //CGI 输出通常不带 Content-Length（你现在也是“流式转发”），要 keep-alive 就得实现 chunked 或缓冲完整长度再发；
    //MVP 先用“发完就关连接”简化状态机，减少 bug 面。
    r->keep_alive = 0;

    /* CGI shares request timeout for MVP */
    zv_add_timer(r, r->request_timeout_ms, zv_http_close_conn);

    return 0;
}
// 处理 CGI 响应可读事件
void zv_cgi_on_stdout_ready(zv_http_request_t *r) {
    if (!r || !r->cgi_active || r->cgi_out_fd < 0) return;
    zv_del_timer(r);

    for (;;) {
        //反压：如果我们还有未发送的正文数据，则停止读取。
        if (r->cgi_body_sent < r->cgi_body_len) {
            break;
        }
        // 读取 CGI 输出 读到的数据放在 r->cgi_body_buf 里
        ssize_t n = read(r->cgi_out_fd, r->cgi_body_buf, sizeof(r->cgi_body_buf));
        if (n > 0) {
            r->cgi_out_total += (size_t)n;// 统计总输出字节数
            // 超出限制，关闭连接
            if (r->cgi_out_total > r->cgi_out_limit) {
                log_warn("cgi output exceeded limit (%zu)", r->cgi_out_limit);
                zv_http_close_conn(r);
                return;
            }
            // 如果没有完成 CGI 头的解析 
            if (!r->cgi_headers_done) {
                /* Accumulate into header buffer until blank line, then treat remainder as body. */
                // 先从r->cgi_body_buf复制能放下的部分到 r->cgi_hdr_buf进行头部解析
                size_t copy = (size_t)n;
                size_t space = sizeof(r->cgi_hdr_buf) - r->cgi_hdr_len;//BUF剩余空间
                if (copy > space) copy = space;// 防止溢出
                memcpy(r->cgi_hdr_buf + r->cgi_hdr_len, r->cgi_body_buf, copy);
                r->cgi_hdr_len += copy;// 更新已用长度

                /* Search for header terminator. */
                char *hdr = r->cgi_hdr_buf;
                size_t hl = r->cgi_hdr_len;
                size_t body_off = 0;
                int found = 0;
                // 查找 \n\n 或 \r\n\r\n
                for (size_t i = 0; i + 1 < hl; i++) {
                    if (hdr[i] == '\n' && hdr[i + 1] == '\n') {
                        body_off = i + 2;
                        found = 1;
                        break;
                    }
                    if (i + 3 < hl && hdr[i] == '\r' && hdr[i + 1] == '\n' && hdr[i + 2] == '\r' && hdr[i + 3] == '\n') {
                        body_off = i + 4;
                        found = 1;
                        break;
                    }
                }
                // 如果找到\n\n 或 \r\n\r\n 代表头部结束
                if (found) {
                    int status = 200;
                    char content_type[256];
                    content_type[0] = '\0';
                    // 解析 CGI 头部内容，提取 Status 和 Content-Type
                    size_t header_len = body_off; //头部的总长度
                    size_t line_start = 0;
                    while (line_start < header_len) {
                        size_t line_end = line_start;
                        //找到hdr[0..body_off) 是"头部（含空行）"中\n位置
                        while (line_end < header_len && hdr[line_end] != '\n') {
                            line_end++;
                        }
                        // 解析这一行
                        size_t len = line_end - line_start;
                        if (len > 0) {
                            char line[512];
                            size_t cp = len;
                            if (cp >= sizeof(line)) {
                                log_warn("cgi header line too long");
                                zv_http_close_conn(r);
                                return;
                            }
                            memcpy(line, hdr + line_start, cp);
                            line[cp] = '\0';
                            // 只解析 Status 和 Content-Type
                            (void)parse_status_code(line, &status);
                            (void)parse_content_type(line, content_type, sizeof(content_type));
                        }
                        // 移动到下一行
                        if (line_end < header_len && hdr[line_end] == '\n') {
                            line_end++;
                        }
                        line_start = line_end;
                    }
                    // 构建真正的 HTTP 响应头
                    if (build_http_header(r, status, content_type) != 0) {
                        zv_http_close_conn(r);
                        return;
                    }
                    r->cgi_headers_done = 1;
                    // 将所有已读取的正文字节移入正文缓冲区以进行发送
                    size_t remain = hl - body_off;
                    if (remain > 0) {
                        if (remain > sizeof(r->cgi_body_buf)) remain = sizeof(r->cgi_body_buf);
                        memmove(r->cgi_body_buf, hdr + body_off, remain);
                        r->cgi_body_len = remain;
                        r->cgi_body_sent = 0;
                    } else {
                        r->cgi_body_len = 0;
                        r->cgi_body_sent = 0;
                    }
                    // 重置 header buffer 长度
                    r->cgi_hdr_len = 0;
                } else {
                    //头部还不够;继续阅读，但如果头部缓冲区满了就退出
                    if (r->cgi_hdr_len >= sizeof(r->cgi_hdr_buf)) {
                        log_warn("cgi header too large");
                        zv_http_close_conn(r);
                        return;
                    }
                    continue;
                }
            } else {
                r->cgi_body_len = (size_t)n;
                r->cgi_body_sent = 0;
            }
            // 数据准备好了 要求写信给客户
            struct epoll_event e;
            e.data.ptr = (void *)r->conn_item;
            e.events = EPOLLOUT | EPOLLET | EPOLLONESHOT;
            zv_epoll_mod(r->epfd, r->fd, &e);

            zv_add_timer(r, r->request_timeout_ms, zv_http_close_conn);
            return;
        }
        // EOF 表示 CGI 子进程已经关闭 stdout（通常也意味着进程快退出了）
        if (n == 0) {
            r->cgi_eof = 1;//告诉写回侧“不会再有更多 body”
            close(r->cgi_out_fd);//关闭 pipe fd
            r->cgi_out_fd = -1;
            break;
        }
        // 信号打断，重试 read。
        if (errno == EINTR) {
            continue;
        }
        // 非阻塞下“暂时没数据”，说明已经读干净了（ET 里这是正确退出点），break 出循环。
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        // 其他错误，关闭连接
        zv_http_close_conn(r);
        return;
    }
    //如果 CGI 标准输出仍然处于活动状态且不是 EOF，则重新启用 CGI 标准输出事件。
    //这次把能读的都读完了，内核缓冲区空了
    //不是“数据不够/空了”，而是“反压：有数据我也先不读”
    /* Re-arm CGI stdout if still active and not EOF. */
    if (r->cgi_active && r->cgi_out_fd >= 0) {
        struct epoll_event ev;
        ev.data.ptr = (void *)r->cgi_out_item;
        ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        zv_epoll_mod(r->epfd, r->cgi_out_fd, &ev);
    }

    zv_add_timer(r, r->request_timeout_ms, zv_http_close_conn);
}
// 处理 CGI 响应可写事件 -1: error  0: finished  1: would block  2: need more reading
int zv_cgi_on_client_writable(zv_http_request_t *r) {
    if (!r || !r->cgi_active) return -1;

    /* Send HTTP header (once) + body buffer. */
    //用 writev() 一次性尽量写 “header + body”
    while (r->cgi_http_header_sent < r->cgi_http_header_len || r->cgi_body_sent < r->cgi_body_len) {
        struct iovec iov[2];
        int iovcnt = 0;
        if (r->cgi_http_header_sent < r->cgi_http_header_len) {
            iov[iovcnt].iov_base = r->cgi_http_header + r->cgi_http_header_sent;
            iov[iovcnt].iov_len = r->cgi_http_header_len - r->cgi_http_header_sent;
            iovcnt++;
        }
        if (r->cgi_body_sent < r->cgi_body_len) {
            iov[iovcnt].iov_base = r->cgi_body_buf + r->cgi_body_sent;
            iov[iovcnt].iov_len = r->cgi_body_len - r->cgi_body_sent;
            iovcnt++;
        }
        // 循环保证写完
        ssize_t n = writev(r->fd, iov, iovcnt);
        if (n > 0) {
            ssize_t left = n;
            if (r->cgi_http_header_sent < r->cgi_http_header_len) {
                size_t hrem = r->cgi_http_header_len - r->cgi_http_header_sent;
                size_t hcons = (left >= (ssize_t)hrem) ? hrem : (size_t)left;
                r->cgi_http_header_sent += hcons;
                left -= (ssize_t)hcons;
            }
            if (left > 0 && r->cgi_body_sent < r->cgi_body_len) {
                size_t brem = r->cgi_body_len - r->cgi_body_sent;
                size_t bcons = (left >= (ssize_t)brem) ? brem : (size_t)left;
                r->cgi_body_sent += bcons;
                left -= (ssize_t)bcons;
            }
            continue;
        }

        if (n == 0) {
            errno = EPIPE;
            return -1;
        }

        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
        return -1;
    }
    //当前准备好的 header+body chunk 都写完了
    /* Sent current buffered body chunk. Reset body buffer. */
    //释放反压  从而 允许继续读取 CGI stdout。
    r->cgi_body_len = 0;
    r->cgi_body_sent = 0;
    //如果 CGI 已经 EOF：可以结束 CGI 生命周期 
    if (r->cgi_eof) {
        /* Reap child (best-effort) and close. */
        if (r->cgi_pid > 0) {
            (void)waitpid(r->cgi_pid, NULL, WNOHANG);
        }
        return 0;
    }
    //如果没 EOF：继续等待更多 CGI 输出
    if (r->cgi_out_fd >= 0) {
        struct epoll_event ev;
        ev.data.ptr = (void *)r->cgi_out_item;
        ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        zv_epoll_mod(r->epfd, r->cgi_out_fd, &ev);
    }

    return 2;
}
