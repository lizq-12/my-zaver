#ifndef UTIL_H
#define UTIL_H

// max number of listen queue
#define LISTENQ     1024
#define BUFLEN      8192
#define DELIM       "="

#define ZV_CONF_OK      0
#define ZV_CONF_ERROR   100

#define MIN(a,b) ((a) < (b) ? (a) : (b))

/* All timeouts use milliseconds and CLOCK_MONOTONIC internally. */
#define ZV_DEFAULT_KEEP_ALIVE_TIMEOUT_MS 5000
#define ZV_DEFAULT_REQUEST_TIMEOUT_MS    5000

struct zv_conf_s {
    void *root;
    int port;
    int thread_num;
    int workers;
    int cpu_affinity;
    int keep_alive_timeout_ms; /* idle connection timeout */
    int request_timeout_ms;    /* in-flight request/response timeout */
};

typedef struct zv_conf_s zv_conf_t;

int open_listenfd_reuseport(int port);
int make_socket_non_blocking(int fd);

int read_conf(char *filename, zv_conf_t *cf, char *buf, int len);
#endif
