
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
#include <stdarg.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/sendfile.h>
#include "http.h"
#include "http_parse.h"
#include "http_request.h"
#include "epoll.h"
#include "error.h"
#include "timer.h"
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
    event.data.ptr = (void *)r;
    event.events = events | EPOLLET | EPOLLONESHOT;
    zv_epoll_mod(r->epfd, r->fd, &event);
}

static const char* get_file_type(const char *type);
static void parse_uri(char *uri, int length, char *filename, char *querystring);
static int prepare_error(zv_http_request_t *r, char *cause, char *errnum, char *shortmsg, char *longmsg, int keep_alive);
static int prepare_static(zv_http_request_t *r, char *filename, size_t filesize, zv_http_out_t *out);
static char *ROOT = NULL;

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
        //读取 URI 并转换成文件路径
        parse_uri(r->uri_start, r->uri_end - r->uri_start, filename, NULL);
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
            zv_add_timer(r, TIMEOUT_DEFAULT, zv_http_close_conn);
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
    zv_add_timer(r, TIMEOUT_DEFAULT, zv_http_close_conn);
    return;

err:
close:
    reset_output(r);
    rc = zv_http_close_conn(r);
    check(rc == 0, "do_request: zv_http_close_conn");
}

void do_write(void *ptr) {
    zv_http_request_t *r = (zv_http_request_t *)ptr;
    int rc;

    zv_del_timer(r);
    rc = try_send(r);

    if (rc == 1) {
        r->writing = 1;
        rearm_event(r, EPOLLOUT);
        zv_add_timer(r, TIMEOUT_DEFAULT, zv_http_close_conn);
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
    zv_add_timer(r, TIMEOUT_DEFAULT, zv_http_close_conn);
}
//uri="/" → filename=ROOT + "/" → 末尾是 / → 最终 ROOT/index.html
//uri="/50x.html" → 末尾段有 . → 最终 ROOT/50x.html
//uri="/docs" → 末尾段没 . → 补 / → 补 index.html → ROOT/docs/index.html
//uri="/img.png?x=1" → file_length 只取到 ? 前 → 最终 ROOT/img.png
static void parse_uri(char *uri, int uri_length, char *filename, char *querystring) {
    check(uri != NULL, "parse_uri: uri is NULL");
    uri[uri_length] = '\0';
    //在 URI 中查找是否有 ? 返回指向它的指针
    char *question_mark = strchr(uri, '?');
    int file_length;
    if (question_mark) {
        file_length = (int)(question_mark - uri);
        debug("file_length = (question_mark - uri) = %d", file_length);
    } else {
        file_length = uri_length;
        debug("file_length = uri_length = %d", file_length);
    }
    //预留：如果你未来想把 ? 后面的 query 保存到 querystring，在这里实现。
    if (querystring) {
        //TODO
    }
    //先把站点根目录拷到 filename，比如 ROOT="/home/.../html"
    strcpy(filename, ROOT);

    // uri_length can not be too long
    // 它检查的是 uri_length，而真正用于拼接的其实是 file_length；而且 filename 里还要先放 ROOT，所以严格来说这不是完全充分的安全检查，但作者的意图是“别让 URI 太长”。
    if (uri_length > (SHORTLINE >> 1)) {
        log_err("uri too long: %.*s", uri_length, uri);
        return;
    }

    debug("before strncat, filename = %s, uri = %.*s, file_len = %d", filename, file_length, uri, file_length);
    //把 URI 的路径部分拼到 ROOT 后面，得到初步文件路径。file_length表示在 URI 中不包含 ? 的路径部分的长度
    strncat(filename, uri, file_length);

    char *last_comp = strrchr(filename, '/');
    char *last_dot = strrchr(last_comp, '.');
    if (last_dot == NULL && filename[strlen(filename)-1] != '/') {
        strcat(filename, "/");
    }
    //如果路径以 / 结尾，说明是目录，补上默认首页文件名
    if(filename[strlen(filename)-1] == '/') {
        strcat(filename, "index.html");
    }

    log_info("filename = %s", filename);
    return;
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
        (void)appendf(r->out_header, sizeof(r->out_header), &header_len, "Keep-Alive: timeout=%d\r\n", TIMEOUT_DEFAULT);
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
        (void)appendf(r->out_header, sizeof(r->out_header), &header_len, "Keep-Alive: timeout=%d\r\n", TIMEOUT_DEFAULT);
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
