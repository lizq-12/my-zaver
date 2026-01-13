
/*
 * Copyright (C) Zhu Jiashun
 * Copyright (C) Zaver
 */

#ifndef _GNU_SOURCE
/* why define _GNU_SOURCE? http://stackoverflow.com/questions/15334558/compiler-gets-warnings-when-using-strptime-function-ci */
#define _GNU_SOURCE
#endif

#include <math.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include "http.h"
#include "http_header_cache.h"
#include "http_request_cache.h"
#include "http_request.h"
#include "error.h"
#include "ep_item.h"

static int zv_http_process_ignore(zv_http_request_t *r, zv_http_out_t *out, char *data, int len);
static int zv_http_process_connection(zv_http_request_t *r, zv_http_out_t *out, char *data, int len);
static int zv_http_process_if_modified_since(zv_http_request_t *r, zv_http_out_t *out, char *data, int len);

zv_http_header_handle_t zv_http_headers_in[] = {
    {"Host", zv_http_process_ignore},
    {"Connection", zv_http_process_connection},
    {"If-Modified-Since", zv_http_process_if_modified_since},
    {"", zv_http_process_ignore}
};
// 初始化 HTTP 请求结构体
int zv_init_request_t(zv_http_request_t *r, int fd, int epfd, zv_conf_t *cf) {
    r->fd = fd;
    r->epfd = epfd;
    r->last = 0;
    r->parse_pos = 0;
    r->request_line_state = 0;
    r->header_state = 0;
    r->parse_phase = 0;
    r->root = cf->root;
    INIT_LIST_HEAD(&(r->list));

    /* reset request-line parsing fields to avoid stale pointers on reuse */
    r->request_start = NULL;
    r->method_end = NULL;
    r->method = ZV_HTTP_UNKNOWN;
    r->uri_start = NULL;
    r->uri_end = NULL;
    r->path_start = NULL;
    r->path_end = NULL;
    r->query_start = NULL;
    r->query_end = NULL;
    r->http_major = 0;
    r->http_minor = 0;
    r->request_end = NULL;

    r->cur_header_key_start = NULL;
    r->cur_header_key_end = NULL;
    r->cur_header_value_start = NULL;
    r->cur_header_value_end = NULL;

    /* timeouts (ms) */
    if (cf) {
        r->keep_alive_timeout_ms = (cf->keep_alive_timeout_ms > 0) ? (size_t)cf->keep_alive_timeout_ms : (size_t)ZV_DEFAULT_KEEP_ALIVE_TIMEOUT_MS;
        r->request_timeout_ms = (cf->request_timeout_ms > 0) ? (size_t)cf->request_timeout_ms : (size_t)ZV_DEFAULT_REQUEST_TIMEOUT_MS;
    } else {
        r->keep_alive_timeout_ms = (size_t)ZV_DEFAULT_KEEP_ALIVE_TIMEOUT_MS;
        r->request_timeout_ms = (size_t)ZV_DEFAULT_REQUEST_TIMEOUT_MS;
    }

    r->timer = NULL;//初始化 timer 为 NULL
    INIT_LIST_HEAD(&(r->freelist));//初始化 freelist 链表头
    // 如果 conn_item 为空 则分配内存
    /* epoll items (allocated once per request block and reused across connections) */
    if (r->conn_item == NULL) {
        r->conn_item = (struct zv_ep_item_s *)malloc(sizeof(struct zv_ep_item_s));
    }
    //默认初始化为连接类型
    if (r->conn_item) {
        zv_ep_item_t *it = (zv_ep_item_t *)r->conn_item;
        it->kind = ZV_EP_KIND_CONN; 
        it->fd = fd;
        it->r = r;
    }
    /* CGI items are allocated on demand; if they already exist, reset them */
    if (r->cgi_out_item) {
        zv_ep_item_t *it = (zv_ep_item_t *)r->cgi_out_item;
        it->kind = ZV_EP_KIND_CGI_OUT;
        it->fd = -1;
        it->r = r;
    }
    if (r->cgi_in_item) {
        zv_ep_item_t *it = (zv_ep_item_t *)r->cgi_in_item;
        it->kind = ZV_EP_KIND_CGI_IN;
        it->fd = -1;
        it->r = r;
    }

    r->keep_alive = 0;
    r->writing = 0;
    r->out_header_len = 0;
    r->out_header_sent = 0;
    r->out_body = NULL;
    r->out_body_len = 0;
    r->out_body_sent = 0;
    r->out_file_fd = -1;
    r->out_file_offset = 0;
    r->out_file_size = 0;
    r->out_header[0] = '\0';

    /* CGI state */
    r->cgi_active = 0;
    r->cgi_pid = -1;
    r->cgi_in_fd = -1;
    r->cgi_out_fd = -1;
    r->cgi_eof = 0;
    r->cgi_out_total = 0;
    r->cgi_out_limit = 1024 * 1024; /* 1 MiB default */
    r->cgi_headers_done = 0;
    r->cgi_hdr_len = 0;
    r->cgi_http_header_len = 0;
    r->cgi_http_header_sent = 0;
    r->cgi_body_len = 0;
    r->cgi_body_sent = 0;

    return ZV_OK;
}
// 释放 HTTP 请求结构体相关资源
int zv_free_request_t(zv_http_request_t *r) {
    list_head *pos, *next;
    zv_http_header_t *hd;
    
    for (pos = r->list.next; pos != &(r->list); pos = next) {
        next = pos->next;
        hd = list_entry(pos, zv_http_header_t, list);
        list_del(pos);
        zv_http_header_free(hd);
    }
    INIT_LIST_HEAD(&(r->list));

    if (r->out_body) {
        free(r->out_body);
        r->out_body = NULL;
    }
    if (r->out_file_fd >= 0) {
        close(r->out_file_fd);
        r->out_file_fd = -1;
    }

    /* CGI cleanup (best-effort) */
    if (r->cgi_active) {
        if (r->cgi_pid > 0) {
            kill(r->cgi_pid, SIGKILL);
            (void)waitpid(r->cgi_pid, NULL, WNOHANG);
        }
        if (r->cgi_in_fd >= 0) {
            close(r->cgi_in_fd);
            r->cgi_in_fd = -1;
        }
        if (r->cgi_out_fd >= 0) {
            close(r->cgi_out_fd);
            r->cgi_out_fd = -1;
        }
        r->cgi_active = 0;
        r->cgi_pid = -1;
    }

    if (r->cgi_out_item) {
        ((zv_ep_item_t *)r->cgi_out_item)->fd = -1;
    }
    if (r->cgi_in_item) {
        ((zv_ep_item_t *)r->cgi_in_item)->fd = -1;
    }

    r->timer = NULL;

    return ZV_OK;
}
// 初始化 HTTP 输出结构体
int zv_init_out_t(zv_http_out_t *o, int fd) {
    o->fd = fd;
    o->keep_alive = 0;
    o->modified = 1;
    o->status = 0;

    return ZV_OK;
}

int zv_free_out_t(zv_http_out_t *o) {
    // TODO
    (void) o;
    return ZV_OK;
}

void zv_http_handle_header(zv_http_request_t *r, zv_http_out_t *o) {
    list_head *pos, *next;
    zv_http_header_t *hd;
    zv_http_header_handle_t *header_in;
    int len;

    /*
     * HTTP keep-alive default behavior:
     * - HTTP/1.1: keep-alive by default unless "Connection: close"
     * - HTTP/1.0: close by default unless "Connection: keep-alive"
     */
    if (r->http_major > 1 || (r->http_major == 1 && r->http_minor >= 1)) {
        o->keep_alive = 1;
    } else {
        o->keep_alive = 0;
    }

    for (pos = r->list.next; pos != &(r->list); pos = next) {
        next = pos->next;
        hd = list_entry(pos, zv_http_header_t, list);
        /* handle */

        for (header_in = zv_http_headers_in; strlen(header_in->name) > 0;header_in++) 
        {
            //
            if (strncmp(hd->key_start, header_in->name, hd->key_end - hd->key_start) == 0) 
            {
                //debug("key = %.*s, value = %.*s", hd->key_end-hd->key_start, hd->key_start, hd->value_end-hd->value_start, hd->value_start);
                len = hd->value_end - hd->value_start;
                (*(header_in->handler))(r, o, hd->value_start, len);
                break;
            }    
        }
        /* delete it from the original list */
        list_del(pos);
        zv_http_header_free(hd);
    }
}
// 关闭 HTTP 连接
int zv_http_close_conn(zv_http_request_t *r) {
    // NOTICE: closing a file descriptor will cause it to be removed from all epoll sets automatically
    // http://stackoverflow.com/questions/8707601/is-it-necessary-to-deregister-a-socket-from-epoll-before-closing-it
    zv_free_request_t(r);
    close(r->fd);
    r->fd = -1;
    zv_http_request_put_deferred(r);

    return ZV_OK;
}

static int zv_http_process_ignore(zv_http_request_t *r, zv_http_out_t *out, char *data, int len) {
    (void) r;
    (void) out;
    (void) data;
    (void) len;
    
    return ZV_OK;
}

static int zv_http_process_connection(zv_http_request_t *r, zv_http_out_t *out, char *data, int len) {
    (void) r;
    if (strncasecmp("keep-alive", data, len) == 0) {
        out->keep_alive = 1;
    } else if (strncasecmp("close", data, len) == 0) {
        out->keep_alive = 0;
    }

    return ZV_OK;
}

static int zv_http_process_if_modified_since(zv_http_request_t *r, zv_http_out_t *out, char *data, int len) {
    (void) r;
    (void) len;

    struct tm tm;
    if (strptime(data, "%a, %d %b %Y %H:%M:%S GMT", &tm) == (char *)NULL) {
        return ZV_OK;
    }
    time_t client_time = mktime(&tm);

    double time_diff = difftime(out->mtime, client_time);
    if (fabs(time_diff) < 1e-6) {
        // log_info("content not modified clienttime = %d, mtime = %d\n", client_time, out->mtime);
        /* Not modified */
        out->modified = 0;
        out->status = ZV_HTTP_NOT_MODIFIED;
    }
    
    return ZV_OK;
}

const char *get_shortmsg_from_status_code(int status_code) {
    /*  for code to msg mapping, please check: 
    * http://users.polytech.unice.fr/~buffa/cours/internet/POLYS/servlets/Servlet-Tutorial-Response-Status-Line.html
    */
    if (status_code == ZV_HTTP_OK) {
        return "OK";
    }

    if (status_code == ZV_HTTP_NOT_MODIFIED) {
        return "Not Modified";
    }

    if (status_code == ZV_HTTP_NOT_FOUND) {
        return "Not Found";
    }
    

    return "Unknown";
}
