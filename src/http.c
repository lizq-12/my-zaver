/*
 * Copyright (C) Zhu Jiashun
 * Copyright (C) Zaver
 */

#include <strings.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdlib.h>
#include <limits.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "http.h"
#include "http_parse.h"
#include "http_request.h"
#include "epoll.h"
#include "error.h"
#include "timer.h"
#include "cgi.h"
/**
 * buf: 目标缓冲区（例如 header 或 body）
 * cap: 缓冲区总容量（通常是 sizeof(header)）
 * len: 当前缓冲区里已使用的长度（不含末尾 \0），函数成功时会更新它
 * fmt: 格式化字符串
 * ...: 可变参数
 */
static int appendf(char *buf, size_t cap, size_t *len, const char *fmt, ...) {
    if (*len >= cap) {
        return -1;
    }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);

    if (n < 0) {
        return -1;
    }

    if ((size_t)n >= cap - *len) {
        *len = cap - 1;
        buf[*len] = '\0';
        return -1;
    }

    *len += (size_t)n;
    return 0;
}

static int is_expected_disconnect_errno(int e) {
    return (e == EPIPE || e == ECONNRESET);
}
// 记录发送失败的日志
static void log_send_failed(const char *where, int fd) {
    if (is_expected_disconnect_errno(errno)) {
        log_warn("%s: client closed, fd=%d, errno=%d", where, fd, errno);
    } else {
        log_err("%s: send failed, fd=%d, errno=%d", where, fd, errno);
    }
}
// 复位记录输出相关状态的字段
static void reset_output(zv_http_request_t *r) {
    r->writing = 0;

    r->out_header_len = 0;
    r->out_header_sent = 0;
    r->out_header[0] = '\0';
    // 因为body是动态分配的，所以需要释放输出 body 相关资源
    if (r->out_body) {
        free(r->out_body);
        r->out_body = NULL;
    }
    r->out_body_len = 0;
    r->out_body_sent = 0;
    // 关闭文件描述符并复位相关字段
    if (r->out_file_fd >= 0) {
        close(r->out_file_fd);
        r->out_file_fd = -1;
    }
    r->out_file_offset = 0;
    r->out_file_size = 0;
}
//发送响应 尝试发送所有数据
// 返回值: 0表示发送完成，1表示未完成需继续发送，-1表示发送出错
static int try_send(zv_http_request_t *r) {
    /* header + optional in-memory body using writev */
    // 发送头部 如果要发body也一并发送
    while (r->out_header_sent < r->out_header_len || (r->out_body && r->out_body_sent < r->out_body_len)) 
    {
        struct iovec iov[2];
        int iovcnt = 0;
        // 发送头部
        if (r->out_header_sent < r->out_header_len) {
            iov[iovcnt].iov_base = r->out_header + r->out_header_sent;
            iov[iovcnt].iov_len = r->out_header_len - r->out_header_sent;
            iovcnt++;
        }
        // 如果body不为空 则发送 body
        if (r->out_body && r->out_body_sent < r->out_body_len) {
            iov[iovcnt].iov_base = r->out_body + r->out_body_sent;
            iov[iovcnt].iov_len = r->out_body_len - r->out_body_sent;
            iovcnt++;
        }
        // 使用 writev 发送多个缓冲区
        // n 表示实际发送的字节数
        ssize_t n = writev(r->fd, iov, iovcnt);
        if (n > 0) {
            ssize_t left = n;
            //writev 可能没有发送完所有数据，所以需要更新已发送的长度
            if (r->out_header_sent < r->out_header_len) {
                size_t hrem = r->out_header_len - r->out_header_sent;// 还剩多少没发送
                size_t hcons = (left >= (ssize_t)hrem) ? hrem : (size_t)left;// 实际发送了多少
                r->out_header_sent += hcons;// 更新已发送长度
                left -= (ssize_t)hcons;
            }
            if (left > 0 && r->out_body && r->out_body_sent < r->out_body_len) {
                size_t brem = r->out_body_len - r->out_body_sent;// 还剩多少没发送
                size_t bcons = (left >= (ssize_t)brem) ? brem : (size_t)left;// 实际发送了多少
                r->out_body_sent += bcons;
                left -= (ssize_t)bcons;
            }
            continue;
        }
        //？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？
        if (n == 0) {
            errno = EPIPE;
            return -1;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 1;
        }

        return -1;
    }
    // 发送文件
    while (r->out_file_fd >= 0 && (size_t)r->out_file_offset < r->out_file_size) 
    {
        off_t off = r->out_file_offset;
        size_t remaining = r->out_file_size - (size_t)r->out_file_offset;
        ssize_t n = sendfile(r->fd, r->out_file_fd, &off, remaining);

        if (n > 0) {
            r->out_file_offset = off;
            continue;
        }
        if (n == 0) {
            r->out_file_offset = off;
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            r->out_file_offset = off;
            return 1;
        }
        return -1;
    }

    return 0;
}
// 修改为输入事件或者输出事件events只能二选一
static void rearm_event(zv_http_request_t *r, uint32_t events) {
    struct epoll_event event;
    event.data.ptr = (void *)r->conn_item;
    event.events = events | EPOLLET | EPOLLONESHOT;
    zv_epoll_mod(r->epfd, r->fd, &event);
}
// 计算 keep-alive 超时时间（秒）（向上取整）
static int keep_alive_timeout_sec(const zv_http_request_t *r) {
    if (!r) return 0;
    size_t ms = r->keep_alive_timeout_ms;
    if (ms == 0) return 0;
    /* HTTP Keep-Alive header uses seconds; round up and clamp to >=1. */
    int sec = (int)((ms + 999) / 1000);
    if (sec < 1) sec = 1;
    return sec;
}

static const char* get_file_type(const char *type);
static int parse_uri(const char *uri, int length, char *filename, size_t filename_cap, char *querystring);
static int prepare_error(zv_http_request_t *r, char *cause, char *errnum, char *shortmsg, char *longmsg, int keep_alive);
static int prepare_static(zv_http_request_t *r, char *filename, size_t filesize, zv_http_out_t *out);
static int percent_decode(const char *in, size_t in_len, char *out, size_t out_cap, size_t *out_len);
static int normalize_abs_path(const char *path, size_t path_len, char *out, size_t out_cap, int *ends_with_slash);
static int is_path_under_root_real(const char *root, const char *path);
static int handle_cgi_mvp(zv_http_request_t *r, int fd, char *filename, size_t filename_cap);
/* handle_cgi_mvp return codes */
#define ZV_CGI_NOT    0// 不是 CGI，请 do_request 继续走静态文件流程
#define ZV_CGI_RETURN 1// 这个请求已经被 CGI 分支“接管”，do_request 应该直接 return
#define ZV_CGI_CLOSE  2// 已经回了错误页且发送完成，do_request 应该 goto close 关连接

static char *ROOT = NULL;

// 将十六进制字符转换为对应的整数值
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}
// 对百分号编码的字符串进行解码
static int percent_decode(const char *in, size_t in_len, char *out, size_t out_cap, size_t *out_len) {
    if (!in || !out || out_cap == 0) return -1;
    size_t j = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char ch = (unsigned char)in[i];
        if (ch == '%') {
            // 需要两个字符来表示一个十六进制数
            if (i + 2 >= in_len) return -1;
            int hi = hex_val(in[i + 1]);// 高四位
            int lo = hex_val(in[i + 2]);// 低四位
            if (hi < 0 || lo < 0) return -1;// 非法的十六进制字符
            ch = (unsigned char)((hi << 4) | lo);// 合并为一个字节
            i += 2;
        }

        /* Basic hardening: reject NUL/backslash and control chars. */
        // 拒绝NUL（\0\）、反斜杠（\）、回车（\r）和换行（\n）
        if (ch == 0 || ch == '\\' || ch == '\r' || ch == '\n') return -1;
        // 检查输出缓冲区是否有足够空间
        if (j + 1 >= out_cap) return -1;
        out[j++] = (char)ch;
    }
    out[j] = '\0';
    if (out_len) *out_len = j;
    return 0;
}
// 规范化绝对路径，处理 . 和 .. 段 合并多余斜杠
// 返回值: 0表示成功，-1表示失败
static int normalize_abs_path(const char *path, size_t path_len, char *out, size_t out_cap, int *ends_with_slash) {
    if (!path || !out || out_cap < 2) return -1;
    if (path_len == 0 || path[0] != '/') return -1;
    // 记录路径是否以斜杠结尾
    int trailing_slash = (path_len > 0 && path[path_len - 1] == '/');

    size_t stack[256];
    size_t sp = 0;

    size_t out_len = 1;
    out[0] = '/';
    out[1] = '\0';

    size_t i = 1;
    while (i < path_len) {
        //跳过多余的斜杠
        while (i < path_len && path[i] == '/') i++;
        if (i >= path_len) break;

        size_t seg_start = i;
        //找到下一个斜杠或者路径结尾
        while (i < path_len && path[i] != '/') i++;
        size_t seg_len = i - seg_start;//这段路径段长度
        if (seg_len == 0) continue;
        // 处理路径段 忽略 . 段
        if (seg_len == 1 && path[seg_start] == '.') {
            continue;
        }
        // 处理 .. 段
        if (seg_len == 2 && path[seg_start] == '.' && path[seg_start + 1] == '.') {
            if (sp == 0) return -1;//如果当前已经在根（sp==0）
            sp--;
            out_len = (sp == 0) ? 1 : stack[sp - 1];// 回退到上一个路径段的末尾
            out[out_len] = '\0';// 截断路径字符串
            continue;
        }
        // 普通路径段  需要添加斜杠分隔符
        if (out_len > 1) {
            if (out_len + 1 >= out_cap) return -1;
            out[out_len++] = '/';
        }
        // 复制路径段
        if (out_len + seg_len >= out_cap) return -1;
        memcpy(out + out_len, path + seg_start, seg_len);
        out_len += seg_len;
        out[out_len] = '\0';
        // 将当前路径段的长度压入栈
        if (sp >= (sizeof(stack) / sizeof(stack[0]))) return -1;
        stack[sp++] = out_len;
    }
    // 如果路径应以斜杠结尾但当前没有，则添加斜杠
    if (trailing_slash && out_len > 1 && out[out_len - 1] != '/') {
        if (out_len + 1 >= out_cap) return -1;
        out[out_len++] = '/';
        out[out_len] = '\0';
    }
    // 设置输出参数是否以斜杠结尾
    if (ends_with_slash) *ends_with_slash = trailing_slash;
    return 0;
}
// 检查路径 path 是否在根目录 root 下（防止目录遍历攻击）
static int is_path_under_root_real(const char *root, const char *path) {
    char root_real[PATH_MAX];
    char path_real[PATH_MAX];

    if (!root || !path) return 0;
    //realpath它把混乱的、带欺骗性的路径，转换成唯一的、绝对的物理路径。
    //如果 path 是 /var/www/html/../../etc/passwd，realpath 会把它变成 /etc/passwd。
    //如果 path 是 /var/www/html/link_to_secret（一个指向外部的软链接），realpath 会直接解析出它指向的真实地址。
    if (!realpath(root, root_real)) return 0;
    if (!realpath(path, path_real)) return 0;

    size_t rlen = strlen(root_real);
    //拿到两个真实路径后，检查 path_real 是否以 root_real 开头。
    if (strncmp(path_real, root_real, rlen) != 0) return 0;
    //边界检查（防止“前缀伪造”攻击）防止strncmp 会比较前 9 个字符（/data/web），发现两者完全一样！于是函数可能错误地返回 1。
    if (path_real[rlen] == '\0' || path_real[rlen] == '/') return 1;
    return 0;
}

mime_type_t zaver_mime[] = 
{
    {".html", "text/html"},
    {".xml", "text/xml"},
    {".xhtml", "application/xhtml+xml"},
    {".txt", "text/plain"},
    {".rtf", "application/rtf"},
    {".pdf", "application/pdf"},
    {".word", "application/msword"},
    {".png", "image/png"},
    {".gif", "image/gif"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".au", "audio/basic"},
    {".mpeg", "video/mpeg"},
    {".mpg", "video/mpeg"},
    {".avi", "video/x-msvideo"},
    {".gz", "application/x-gzip"},
    {".tar", "application/x-tar"},
    {".css", "text/css"},
    {NULL ,"text/plain"}
};

void do_request(void *ptr) {
    zv_http_request_t *r = (zv_http_request_t *)ptr;
    int fd = r->fd;
    int rc, n;
    char filename[SHORTLINE];
    struct stat sbuf;
    ROOT = r->root;
    char *plast = NULL;
    size_t remain_size;
    
    zv_del_timer(r);
    for(;;) 
    {
        //如果缓冲区没有数据了 才能继续读取
        if (r->parse_pos >= r->last) {
            //由于零拷贝这里最多读取 MAX_BUF - 1 字节
            if (r->last >= MAX_BUF - 1) {
                log_err("request buffer overflow!");
                goto err;
            }

            plast = &r->buf[r->last];
            remain_size = (size_t)(MAX_BUF - r->last - 1);
            n = read(fd, plast, remain_size);
            check(r->last < MAX_BUF, "request buffer overflow!");

            if (n == 0) {
                // EOF
                log_info("read return 0, ready to close fd %d, remain_size = %zu", fd, remain_size);
                goto err;
            }

            if (n < 0) {
                if (errno != EAGAIN) {
                    log_err("read err, and errno = %d", errno);
                    goto err;
                }
                break;
            }

            r->last += n;
            check(r->last < MAX_BUF, "request buffer overflow!");
        }
        //解析阶段的状态机
        if (r->parse_phase == 0) {
            log_info("ready to parse request line");
            rc = zv_http_parse_request_line(r);
            if (rc == ZV_AGAIN) {
                continue;
            } else if (rc != ZV_OK) {
                log_err("rc != ZV_OK");
                goto err;
            }
            r->parse_phase = 1;

            log_info("method == %.*s", (int)(r->method_end - r->request_start), (char *)r->request_start);
            log_info("uri == %.*s", (int)(r->uri_end - r->uri_start), (char *)r->uri_start);
        }

        if (r->parse_phase == 1) {
            debug("ready to parse request body");
            rc = zv_http_parse_request_body(r);
            if (rc == ZV_AGAIN) {
                continue;
            } else if (rc != ZV_OK) {
                log_err("rc != ZV_OK");
                goto err;
            }
            r->parse_phase = 2;
        }
        //至此已经完整解析了一个 HTTP 请求
        // 处理 CGI 请求
        rc = handle_cgi_mvp(r, fd, filename, sizeof(filename));
        if (rc < 0) {
            goto err;
        }
        if (rc == ZV_CGI_RETURN) {
            return;
        }
        if (rc == ZV_CGI_CLOSE) {
            goto close;
        }

        //读取 URI 并转换成文件路径
        if (parse_uri(r->uri_start, (int)(r->uri_end - r->uri_start), filename, sizeof(filename), NULL) < 0) {
            rc = prepare_error(r, "invalid uri", "400", "Bad Request", "invalid uri", 0);
            if (rc < 0) {
                goto err;
            }
            rc = try_send(r);
            if (rc < 0) {
                log_send_failed("try_send 400", fd);
                goto err;
            }
            if (rc == 1) {
                r->writing = 1;
                rearm_event(r, EPOLLOUT);
                zv_add_timer(r, r->request_timeout_ms, zv_http_close_conn);
                return;
            }
            reset_output(r);
            goto close;
        }
        //为响应分配并初始化输出结构体
        zv_http_out_t *out = (zv_http_out_t *)malloc(sizeof(zv_http_out_t));
        if (out == NULL) {
            log_err("no enough space for zv_http_out_t");
            exit(1);
        }
        rc = zv_init_out_t(out, fd);
        check(rc == ZV_OK, "zv_init_out_t");
        //根据请求头设置 out 结构体成员
        zv_http_handle_header(r, out);
        check(list_empty(&(r->list)) == 1, "header list should be empty");
        //获取文件状态
        if(stat(filename, &sbuf) < 0) {
            rc = prepare_error(r, filename, "404", "Not Found", "zaver can't find the file", out->keep_alive);
            if (rc < 0) {
                free(out);
                goto err;
            }
            rc = try_send(r);
            if (rc < 0) {
                log_send_failed("try_send 404", fd);
                free(out);
                goto err;
            }
            if (rc == 1) {
                r->writing = 1;
            } else {
                reset_output(r);
            }
            goto request_done;
        }
        //检查文件路径是否在根目录下
        if (!is_path_under_root_real(ROOT, filename)) {
            rc = prepare_error(r, filename, "403", "Forbidden", "path is outside docroot", out->keep_alive);
            if (rc < 0) {
                free(out);
                goto err;
            }
            rc = try_send(r);
            if (rc < 0) {
                log_send_failed("try_send 403(root)", fd);
                free(out);
                goto err;
            }
            if (rc == 1) {
                r->writing = 1;
            } else {
                reset_output(r);
            }
            goto request_done;
        }
        //判断是否为普通文件  并且当前用户是否有读取权限
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode))
        {
            rc = prepare_error(r, filename, "403", "Forbidden",
                    "zaver can't read the file", out->keep_alive);
            if (rc < 0) {
                free(out);
                goto err;
            }
            rc = try_send(r);
            if (rc < 0) {
                log_send_failed("try_send 403", fd);
                free(out);
                goto err;
            }
            if (rc == 1) {
                r->writing = 1;
            } else {
                reset_output(r);
            }
            goto request_done;
        }
        //初始化 out 结构体的 mtime 和 status 成员
        out->mtime = sbuf.st_mtime;
        // 如果之前没有被设置状态码 则设置为 200 OK
        if (out->status == 0) {
            out->status = ZV_HTTP_OK;
        }
        // 发送静态文件
        rc = prepare_static(r, filename, sbuf.st_size, out);
        if (rc < 0) {
            free(out);
            goto err;
        }
        rc = try_send(r);
        if (rc < 0) {
            log_send_failed("try_send static", fd);
            free(out);
            goto err;
        }
        if (rc == 1) {
            r->writing = 1;
        } else {
            reset_output(r);
        }

request_done:
        /*
         * Current request fully handled.
         * Compact any pipelined bytes [parse_pos, last) to the buffer head and reset parser state.
         */
        if (r->parse_pos > r->last) {
            log_err("invalid parse_pos");
            free(out);
            goto err;
        }

        if (r->parse_pos > 0) {
            size_t remaining = r->last - r->parse_pos;
            if (remaining > 0) {
                memmove(r->buf, r->buf + r->parse_pos, remaining);
            }
            r->last = remaining;
        }

        r->parse_pos = 0;
        r->request_line_state = 0;
        r->header_state = 0;
        r->parse_phase = 0;
        r->request_start = NULL;
        r->method_end = NULL;
        r->uri_start = NULL;
        r->uri_end = NULL;
        r->request_end = NULL;

        if (r->writing) {
            free(out);
            rearm_event(r, EPOLLOUT);
            zv_add_timer(r, r->request_timeout_ms, zv_http_close_conn);
            return;
        }

        if (!out->keep_alive) {
            log_info("no keep_alive! ready to close");
            free(out);
            goto close;
        }
        free(out);
    }
    
    rearm_event(r, EPOLLIN);
    /* If we already buffered some request data (or are mid-parse), treat it as in-flight. */
    size_t tmo = (r->last > 0 || r->parse_phase != 0) ? r->request_timeout_ms : r->keep_alive_timeout_ms;
    zv_add_timer(r, tmo, zv_http_close_conn);
    return;

err:
close:
    reset_output(r);
    rc = zv_http_close_conn(r);
    check(rc == 0, "do_request: zv_http_close_conn");
}
// 处理最简版 CGI 请求
//-1 表示出错 错误页
//ZV_CGI_NOT 不是 CGI，请 do_request 继续走静态文件流程
//ZV_CGI_RETURN 这个请求已经被 CGI 分支“接管”，do_request 应该直接 return
//ZV_CGI_CLOSE 已经回了错误页且发送完成，do_request 应该 goto close 关连接
static int handle_cgi_mvp(zv_http_request_t *r, int fd, char *filename, size_t filename_cap) {
    if (!r || !r->uri_start || !r->uri_end || !filename || filename_cap == 0) {
        return ZV_CGI_NOT;//这里返回 ZV_CGI_NOT 而不是报错，是为了不改变主流程的容错：主流程后面会对 URI 做自己的检查并回 400。
    }

    int rc;
    int uri_len = (int)(r->uri_end - r->uri_start);
    int path_len = uri_len;
    for (int i = 0; i < uri_len; i++) {
        if (((char *)r->uri_start)[i] == '?') {
            path_len = i;
            break;
        }
    }
    // 检查 URI 是否以 /cgi-bin/ 开头
    if (path_len < 9 || strncmp((const char *)r->uri_start, "/cgi-bin/", 9) != 0) {
        return ZV_CGI_NOT;
    }
    // 仅支持 GET 方法 如果 CGI 不支持的 method，就当普通错误响应处理。
    if (r->method != ZV_HTTP_GET) {
        rc = prepare_error(r, "cgi", "405", "Method Not Allowed", "cgi GET only", 0);
        if (rc < 0) {
            return -1;
        }
        rc = try_send(r);
        if (rc < 0) {
            log_send_failed("try_send 405", fd);
            return -1;
        }
        if (rc == 1) {
            r->writing = 1;
            rearm_event(r, EPOLLOUT);
            zv_add_timer(r, r->request_timeout_ms, zv_http_close_conn);
            return ZV_CGI_RETURN;
        }
        return ZV_CGI_CLOSE;
    }
    // 消费并清空 header list（这里不使用但是需要消费掉清空）
    /* Parse request headers (keep-alive etc). CGI MVP will force close anyway. */
    zv_http_out_t tmp_out;
    (void)zv_init_out_t(&tmp_out, fd);
    zv_http_handle_header(r, &tmp_out);
    check(list_empty(&(r->list)) == 1, "header list should be empty");

    /* Build SCRIPT_NAME and QUERY_STRING */
    char script_name[SHORTLINE];
    // 检查脚本名长度是否超过缓冲区容量 出错处理
    if (path_len >= (int)sizeof(script_name)) {
        rc = prepare_error(r, "cgi uri too long", "400", "Bad Request", "cgi uri too long", 0);
        if (rc < 0) {
            return -1;
        }
        rc = try_send(r);
        if (rc < 0) {
            log_send_failed("try_send 400(cgi)", fd);
            return -1;
        }
        if (rc == 1) {
            r->writing = 1;
            rearm_event(r, EPOLLOUT);
            zv_add_timer(r, r->request_timeout_ms, zv_http_close_conn);
            return ZV_CGI_RETURN;
        }
        return ZV_CGI_CLOSE;
    }
    // 复制脚本名
    memcpy(script_name, r->uri_start, (size_t)path_len);
    script_name[path_len] = '\0';
    // qs 是 ? 后面的原始 query（不做 decode），同样交给 CGI env。
    char query_string[SHORTLINE];
    const char *qs = NULL;
    if (path_len < uri_len) {
        int qlen = uri_len - path_len - 1;
        if (qlen > 0) {
            if (qlen >= (int)sizeof(query_string)) {
                qlen = (int)sizeof(query_string) - 1;
            }
            memcpy(query_string, (char *)r->uri_start + path_len + 1, (size_t)qlen);
            query_string[qlen] = '\0';
            qs = query_string;
        }
    }

    /* Map URI path to filesystem under docroot, without index.html heuristic. */
    char decoded[SHORTLINE];
    size_t decoded_len = 0;
    if (percent_decode((const char *)r->uri_start, (size_t)path_len, decoded, sizeof(decoded), &decoded_len) < 0) {
        rc = prepare_error(r, "invalid cgi uri", "400", "Bad Request", "invalid cgi uri", 0);
        if (rc < 0) {
            return -1;
        }
        rc = try_send(r);
        if (rc < 0) {
            log_send_failed("try_send 400(cgi uri)", fd);
            return -1;
        }
        if (rc == 1) {
            r->writing = 1;
            rearm_event(r, EPOLLOUT);
            zv_add_timer(r, r->request_timeout_ms, zv_http_close_conn);
            return ZV_CGI_RETURN;
        }
        return ZV_CGI_CLOSE;
    }

    char norm[SHORTLINE];
    if (normalize_abs_path(decoded, decoded_len, norm, sizeof(norm), NULL) < 0) {
        rc = prepare_error(r, "invalid cgi uri", "400", "Bad Request", "invalid cgi uri", 0);
        if (rc < 0) {
            return -1;
        }
        rc = try_send(r);
        if (rc < 0) {
            log_send_failed("try_send 400(cgi norm)", fd);
            return -1;
        }
        if (rc == 1) {
            r->writing = 1;
            rearm_event(r, EPOLLOUT);
            zv_add_timer(r, r->request_timeout_ms, zv_http_close_conn);
            return ZV_CGI_RETURN;
        }
        return ZV_CGI_CLOSE;
    }
    // 确保规范化后仍在 /cgi-bin/ 下（防止编码绕过）
    if (strncmp(norm, "/cgi-bin/", 9) != 0) {
        rc = prepare_error(r, "invalid cgi uri", "403", "Forbidden", "invalid cgi uri", 0);
        if (rc < 0) {
            return -1;
        }
        rc = try_send(r);
        if (rc < 0) {
            log_send_failed("try_send 403(cgi)", fd);
            return -1;
        }
        if (rc == 1) {
            r->writing = 1;
            rearm_event(r, EPOLLOUT);
            zv_add_timer(r, r->request_timeout_ms, zv_http_close_conn);
            return ZV_CGI_RETURN;
        }
        return ZV_CGI_CLOSE;
    }
    // 拼成磁盘路径 ？？？？？？？？？？？？？？？？？？？？？？？？？
    int nfmt = snprintf(filename, filename_cap, "%s%s", ROOT, norm);
    if (nfmt < 0 || (size_t)nfmt >= filename_cap) {
        rc = prepare_error(r, "cgi path too long", "400", "Bad Request", "cgi path too long", 0);
        if (rc < 0) {
            return -1;
        }
        rc = try_send(r);
        if (rc < 0) {
            log_send_failed("try_send 400(cgi path)", fd);
            return -1;
        }
        if (rc == 1) {
            r->writing = 1;
            rearm_event(r, EPOLLOUT);
            zv_add_timer(r, r->request_timeout_ms, zv_http_close_conn);
            return ZV_CGI_RETURN;
        }
        return ZV_CGI_CLOSE;
    }
    //必须是普通文件且可执行
    struct stat cgi_sb;
    if (stat(filename, &cgi_sb) < 0 || !S_ISREG(cgi_sb.st_mode) || !(cgi_sb.st_mode & S_IXUSR)) {
        rc = prepare_error(r, filename, "404", "Not Found", "cgi not found", 0);
        if (rc < 0) {
            return -1;
        }
        rc = try_send(r);
        if (rc < 0) {
            log_send_failed("try_send 404(cgi)", fd);
            return -1;
        }
        if (rc == 1) {
            r->writing = 1;
            rearm_event(r, EPOLLOUT);
            zv_add_timer(r, r->request_timeout_ms, zv_http_close_conn);
            return ZV_CGI_RETURN;
        }
        return ZV_CGI_CLOSE;
    }
    // realpath 约束，防止软链接逃逸到 docroot 外
    if (!is_path_under_root_real(ROOT, filename)) {
        rc = prepare_error(r, filename, "403", "Forbidden", "cgi path is outside docroot", 0);
        if (rc < 0) {
            return -1;
        }
        rc = try_send(r);
        if (rc < 0) {
            log_send_failed("try_send 403(cgi root)", fd);
            return -1;
        }
        if (rc == 1) {
            r->writing = 1;
            rearm_event(r, EPOLLOUT);
            zv_add_timer(r, r->request_timeout_ms, zv_http_close_conn);
            return ZV_CGI_RETURN;
        }
        return ZV_CGI_CLOSE;
    }
    // 启动 CGI 进程
    if (zv_cgi_start(r, filename, script_name, qs) != 0) {
        rc = prepare_error(r, "cgi", "500", "Internal Server Error", "cgi start failed", 0);
        if (rc < 0) {
            return -1;
        }
        rc = try_send(r);
        if (rc < 0) {
            log_send_failed("try_send 500(cgi)", fd);
            return -1;
        }
        if (rc == 1) {
            r->writing = 1;
            rearm_event(r, EPOLLOUT);
            zv_add_timer(r, r->request_timeout_ms, zv_http_close_conn);
            return ZV_CGI_RETURN;
        }
        return ZV_CGI_CLOSE;
    }

    return ZV_CGI_RETURN;
}

void do_write(void *ptr) {
    zv_http_request_t *r = (zv_http_request_t *)ptr;
    int rc;

    zv_del_timer(r);
    // 如果是 CGI 响应 则调用 CGI 写处理函数
    if (r->cgi_active) {
        int c = zv_cgi_on_client_writable(r);
        if (c == 1) {
            /* EAGAIN */
            rearm_event(r, EPOLLOUT);
            zv_add_timer(r, r->request_timeout_ms, zv_http_close_conn);
            return;
        }
        if (c == 2) {
            /* waiting for more CGI output */
            zv_add_timer(r, r->request_timeout_ms, zv_http_close_conn);
            return;
        }
        /* done or error */
        zv_http_close_conn(r);
        return;
    }
    rc = try_send(r);

    if (rc == 1) {
        r->writing = 1;
        rearm_event(r, EPOLLOUT);
        zv_add_timer(r, r->request_timeout_ms, zv_http_close_conn);
        return;
    }

    if (rc < 0) {
        if (is_expected_disconnect_errno(errno)) {
            log_warn("client closed while writing response, fd=%d", r->fd);
        } else {
            log_err("write response failed, fd=%d", r->fd);
        }
        reset_output(r);
        zv_http_close_conn(r);
        return;
    }

    /* done */
    r->writing = 0;
    reset_output(r);

    if (!r->keep_alive) {
        zv_http_close_conn(r);
        return;
    }

    rearm_event(r, EPOLLIN);
    zv_add_timer(r, r->keep_alive_timeout_ms, zv_http_close_conn);
}
//uri="/" → filename=ROOT + "/" → 末尾是 / → 最终 ROOT/index.html
//uri="/50x.html" → 末尾段有 . → 最终 ROOT/50x.html
//uri="/docs" → 末尾段没 . → 补 / → 补 index.html → ROOT/docs/index.html
//uri="/img.png?x=1" → file_length 只取到 ? 前 → 最终 ROOT/img.png
static int parse_uri(const char *uri, int uri_length, char *filename, size_t filename_cap, char *querystring) {
    (void)querystring;
    if (!uri || !filename || filename_cap == 0 || !ROOT) {
        return -1;
    }
    if (uri_length <= 0) {
        return -1;
    }
    if ((size_t)uri_length > (SHORTLINE >> 1)) {
        log_err("uri too long");
        return -1;
    }

    int path_len = uri_length;
    // 它先扫描 ?，只取 ? 前面的部分当 path
    for (int i = 0; i < uri_length; i++) {
        if (uri[i] == '?') {
            path_len = i;
            break;
        }
    }
    if (path_len <= 0) {
        return -1;
    }

    char decoded[SHORTLINE];
    size_t decoded_len = 0;
    // 先对 path 部分进行百分号解码同时检查合法性
    if (percent_decode(uri, (size_t)path_len, decoded, sizeof(decoded), &decoded_len) < 0) {
        return -1;
    }
    if (decoded_len == 0 || decoded[0] != '/') {// 必须是绝对路径
        return -1;
    }
    // 规范化绝对路径，处理 . 和 .. 段 合并多余斜杠
    char norm[SHORTLINE];
    if (normalize_abs_path(decoded, decoded_len, norm, sizeof(norm), NULL) < 0) {
        return -1;
    }

    char path_final[SHORTLINE];
    size_t path_final_len = 0;
    path_final[0] = '\0';

    size_t norm_len = strlen(norm);
    if (norm_len == 0) {
        return -1;
    }
    // 
    int is_dir = (norm_len > 0 && norm[norm_len - 1] == '/');
    if (!is_dir) {// 不是目录 则检查最后一个路径段是否有扩展名
        const char *last_slash = strrchr(norm, '/');
        const char *last_comp = last_slash ? (last_slash + 1) : norm;
        if (last_comp && strchr(last_comp, '.') == NULL) {// 没有扩展名 则补上斜杠
            if (snprintf(path_final, sizeof(path_final), "%s/", norm) < 0) return -1;
        } else {// 有扩展名 直接复制
            if (snprintf(path_final, sizeof(path_final), "%s", norm) < 0) return -1;
        }
    } else {// 是目录 直接复制
        if (snprintf(path_final, sizeof(path_final), "%s", norm) < 0) return -1;
    }
    // 如果 path_final 以斜杠结尾 则补上 index.html
    path_final_len = strlen(path_final);
    if (path_final_len == 0) return -1;
    if (path_final[path_final_len - 1] == '/') {
        if (path_final_len + strlen("index.html") + 1 >= sizeof(path_final)) return -1;
        strcat(path_final, "index.html");
    }
    // 拼接 ROOT 和 path_final 得到最终的文件路径
    const char *root = ROOT;
    size_t root_len = strlen(root);
    const char *path_part = path_final;
    // 避免出现双斜杠
    if (root_len > 0 && root[root_len - 1] == '/' && path_part[0] == '/') {
        path_part = path_part + 1;
    }
    // 拼接得到最终文件路径
    int n = snprintf(filename, filename_cap, "%s%s", root, path_part);
    if (n < 0 || (size_t)n >= filename_cap) {
        return -1;
    }

    log_info("filename = %s", filename);
    return 0;
}
// 准备错误响应（由 try_send/do_write 负责真正发送）
static int prepare_error(zv_http_request_t *r, char *cause, char *errnum, char *shortmsg, char *longmsg, int keep_alive)
{
    char body_tmp[MAXLINE];
    size_t header_len = 0;
    size_t body_len = 0;

    reset_output(r);
    r->keep_alive = keep_alive;

    body_tmp[0] = '\0';
    (void)appendf(body_tmp, sizeof(body_tmp), &body_len, "<html><title>Zaver Error</title>");
    (void)appendf(body_tmp, sizeof(body_tmp), &body_len, "<body bgcolor=\"ffffff\">\n");
    (void)appendf(body_tmp, sizeof(body_tmp), &body_len, "%s: %s\n", errnum, shortmsg);
    (void)appendf(body_tmp, sizeof(body_tmp), &body_len, "<p>%s: %s\n</p>", longmsg, cause);
    (void)appendf(body_tmp, sizeof(body_tmp), &body_len, "<hr><em>Zaver web server</em>\n</body></html>");

    r->out_body = (char *)malloc(body_len);
    if (!r->out_body) {
        return -1;
    }
    memcpy(r->out_body, body_tmp, body_len);
    r->out_body_len = body_len;
    r->out_body_sent = 0;

    r->out_header[0] = '\0';
    (void)appendf(r->out_header, sizeof(r->out_header), &header_len, "HTTP/1.1 %s %s\r\n", errnum, shortmsg);
    (void)appendf(r->out_header, sizeof(r->out_header), &header_len, "Server: Zaver\r\n");
    (void)appendf(r->out_header, sizeof(r->out_header), &header_len, "Content-type: text/html\r\n");

    if (keep_alive) {
        (void)appendf(r->out_header, sizeof(r->out_header), &header_len, "Connection: keep-alive\r\n");
        (void)appendf(r->out_header, sizeof(r->out_header), &header_len, "Keep-Alive: timeout=%d\r\n", keep_alive_timeout_sec(r));
    } else {
        (void)appendf(r->out_header, sizeof(r->out_header), &header_len, "Connection: close\r\n");
    }

    (void)appendf(r->out_header, sizeof(r->out_header), &header_len, "Content-length: %zu\r\n\r\n", body_len);
    r->out_header_len = header_len;
    r->out_header_sent = 0;
    r->out_file_fd = -1;
    return 0;
}

// 准备静态文件响应（由 try_send/do_write 负责真正发送）//sprintf会带上\0
static int prepare_static(zv_http_request_t *r, char *filename, size_t filesize, zv_http_out_t *out) {
    char buf[SHORTLINE];
    size_t header_len = 0;
    struct tm tm;
    
    const char *file_type;
    const char *dot_pos = strrchr(filename, '.');
    file_type = get_file_type(dot_pos);//获取文件类型
    
    reset_output(r);
    r->keep_alive = out->keep_alive;

    r->out_header[0] = '\0';
    (void)appendf(r->out_header, sizeof(r->out_header), &header_len, "HTTP/1.1 %d %s\r\n", out->status, get_shortmsg_from_status_code(out->status));
   
    if (out->keep_alive) {
        (void)appendf(r->out_header, sizeof(r->out_header), &header_len, "Connection: keep-alive\r\n");
        (void)appendf(r->out_header, sizeof(r->out_header), &header_len, "Keep-Alive: timeout=%d\r\n", keep_alive_timeout_sec(r));
    } else {
        (void)appendf(r->out_header, sizeof(r->out_header), &header_len, "Connection: close\r\n");
    }
    // 如果文件被修改过，才发送文件相关的头信息
    if (out->modified) {
        (void)appendf(r->out_header, sizeof(r->out_header), &header_len, "Content-type: %s\r\n", file_type);
        (void)appendf(r->out_header, sizeof(r->out_header), &header_len, "Content-length: %zu\r\n", filesize);
        localtime_r(&(out->mtime), &tm);
        strftime(buf, SHORTLINE,  "%a, %d %b %Y %H:%M:%S GMT", &tm);
        (void)appendf(r->out_header, sizeof(r->out_header), &header_len, "Last-Modified: %s\r\n", buf);
    }

    (void)appendf(r->out_header, sizeof(r->out_header), &header_len, "Server: Zaver\r\n");
    (void)appendf(r->out_header, sizeof(r->out_header), &header_len, "\r\n");// 空行，结束头部
    r->out_header_len = header_len;
    r->out_header_sent = 0;

    if (!out->modified) {
        r->out_file_fd = -1;
        r->out_file_offset = 0;
        r->out_file_size = 0;
        return 0;
    }

    int srcfd = open(filename, O_RDONLY, 0);
    if (srcfd < 0) {
        return -1;
    }
    r->out_file_fd = srcfd;
    r->out_file_offset = 0;
    r->out_file_size = filesize;
    return 0;
}
// 根据文件扩展名获取对应的 MIME 类型
static const char* get_file_type(const char *type)
{
    if (type == NULL) {
        return "text/plain";
    }

    int i;
    for (i = 0; zaver_mime[i].type != NULL; ++i) {
        if (strcmp(type, zaver_mime[i].type) == 0)
            return zaver_mime[i].value;
    }
    return zaver_mime[i].value;
}
