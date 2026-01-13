#ifndef ZV_EP_ITEM_H
#define ZV_EP_ITEM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration to avoid circular includes. */
struct zv_http_request_s;

typedef enum {
    ZV_EP_KIND_LISTEN = 1,//
    ZV_EP_KIND_CONN = 2,
    ZV_EP_KIND_CGI_OUT = 3,
    ZV_EP_KIND_CGI_IN = 4
} zv_ep_kind_t;

typedef struct zv_ep_item_s {
    zv_ep_kind_t kind;// 事件类型
    int fd;// 关联的文件描述符
    struct zv_http_request_s *r;// 关联的请求结构体
} zv_ep_item_t;

#ifdef __cplusplus
}
#endif

#endif
