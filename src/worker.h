/*
 * Zaver worker process (epoll loop)
 */

#ifndef ZV_WORKER_H
#define ZV_WORKER_H

#include "util.h"

int zv_worker_run(zv_conf_t *cf, int worker_id);

#endif
