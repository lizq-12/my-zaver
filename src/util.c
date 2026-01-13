
/*
 * Copyright (C) Zhu Jiashun
 * Copyright (C) Zaver
 */

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include "util.h"
#include "dbg.h"

// 打开一个监听port的套接字，启用SO_REUSEPORT选项（用于多进程工作者）。
// 注意：SO_REUSEPORT必须在bind()之前设置。
int open_listenfd_reuseport(int port)
{
    // 默认端口3000
    if (port <= 0) {
        port = 3000;
    }
    int listenfd, optval = 1;
    struct sockaddr_in serveraddr;

    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;
// 设置地址复用选项
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
                   (const void *)&optval, sizeof(int)) < 0)
        return -1;
// 设置端口复用选项
#ifdef SO_REUSEPORT
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEPORT,
                   (const void *)&optval, sizeof(int)) < 0)
        return -1;
#else
    errno = ENOPROTOOPT;
    return -1;
#endif

    bzero((char *)&serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)port);
    if (bind(listenfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0)
        return -1;

    if (listen(listenfd, LISTENQ) < 0)
        return -1;

    return listenfd;
}
// 将套接字设置为非阻塞模式
int make_socket_non_blocking(int fd) {
    int flags, s;
    flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        log_err("fcntl");
        return -1;
    }

    flags |= O_NONBLOCK;
    s = fcntl(fd, F_SETFL, flags);
    if (s == -1) {
        log_err("fcntl");
        return -1;
    }

    return 0;
}

/*
* Read configuration file
* TODO: trim input line
*/
int read_conf(char *filename, zv_conf_t *cf, char *buf, int len) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        log_err("cannot open config file: %s", filename);
        return ZV_CONF_ERROR;
    }
    //默认配置
    cf->root = NULL;
    cf->port = 3000;
    cf->thread_num = 4;
    cf->workers = 1;
    cf->cpu_affinity = 0;
    cf->keep_alive_timeout_ms = ZV_DEFAULT_KEEP_ALIVE_TIMEOUT_MS;
    cf->request_timeout_ms = ZV_DEFAULT_REQUEST_TIMEOUT_MS;

    int pos = 0;
    char *delim_pos;
    int line_len;
    char *cur_pos = buf+pos;
    while (fgets(cur_pos, len-pos, fp)) {
        delim_pos = strstr(cur_pos, DELIM);
        line_len = strlen(cur_pos);

        //debug("read one line from conf: %s, len = %d", cur_pos, line_len);
        if (!delim_pos)
            return ZV_CONF_ERROR;

        size_t cur_len = strlen(cur_pos);
        if (cur_len > 0 && cur_pos[cur_len - 1] == '\n') {
            cur_pos[cur_len - 1] = '\0';
            cur_len--;
        }
        while (cur_len > 0 && (cur_pos[cur_len - 1] == '\r' || cur_pos[cur_len - 1] == ' ' || cur_pos[cur_len - 1] == '\t')) {
            cur_pos[cur_len - 1] = '\0';
            cur_len--;
        }

        char *val = delim_pos + 1;
        while (*val == ' ' || *val == '\t') {
            val++;
        }

        if (strncmp("root", cur_pos, 4) == 0) {
            cf->root = val;
        }

        if (strncmp("port", cur_pos, 4) == 0) {
            cf->port = atoi(val);
        }

        if (strncmp("threadnum", cur_pos, 9) == 0) {
            cf->thread_num = atoi(val);
        }

        if (strncmp("workers", cur_pos, 7) == 0) {
            cf->workers = atoi(val);
        }

        if (strncmp("cpu_affinity", cur_pos, 12) == 0) {
            cf->cpu_affinity = atoi(val);
        }

        if (strncmp("keep_alive_timeout_ms", cur_pos, 20) == 0) {
            cf->keep_alive_timeout_ms = atoi(val);
        }

        if (strncmp("request_timeout_ms", cur_pos, 18) == 0) {
            cf->request_timeout_ms = atoi(val);
        }

        /* alias: set both timeouts */
        if (strncmp("timeout_ms", cur_pos, 10) == 0) {
            int t = atoi(val);
            cf->keep_alive_timeout_ms = t;
            cf->request_timeout_ms = t;
        }

        cur_pos += line_len;
        pos += line_len;
    }

    fclose(fp);
    return ZV_CONF_OK;
}