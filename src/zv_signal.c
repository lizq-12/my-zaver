/*
 * Zaver signal helpers (master/worker stop flag)
 */

#include "zv_signal.h"

#include <string.h>

volatile sig_atomic_t zv_stop = 0;
// SIGTERM 和 SIGINT 信号处理函数 设置停止标志
static void on_term(int signo) {
    (void)signo;
    zv_stop = 1;
}
//处理 SIGTERM 和 SIGINT 信号的通用安装函数
static int install_common(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGTERM, &sa, NULL) != 0) return -1;
    if (sigaction(SIGINT, &sa, NULL) != 0) return -1;
    return 0;
}
// 安装 master 进程的信号处理函数
int zv_install_master_signals(void) {
    return install_common();
}
// 安装 worker 进程的信号处理函数
int zv_install_worker_signals(void) {
    return install_common();
}
