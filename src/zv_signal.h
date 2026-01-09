/*
 * Zaver signal helpers (master/worker stop flag)
 */

#ifndef ZV_SIGNAL_H
#define ZV_SIGNAL_H

#include <signal.h>

extern volatile sig_atomic_t zv_stop;

int zv_install_master_signals(void);
int zv_install_worker_signals(void);

#endif
