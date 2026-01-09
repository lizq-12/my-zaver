/*
 * Zaver master/worker process management
 */
#include "process.h"
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "zv_signal.h"
#include "worker.h"
#include "dbg.h"

static int cpu_count(void) {
#ifdef _SC_NPROCESSORS_ONLN
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0 && n < 1024) return (int)n;
#endif
    return 1;
}
// 启动服务器 主进程创建多个worker子进程
int zv_run_server(zv_conf_t *cf) {
    // 读取配置文件中的worker数量
    int workers = cf->workers;
    if (workers == 0) {
        workers = cpu_count();// 默认与CPU核数相同
    }
    if (workers < 0) {
        workers = cpu_count();
    }
    // 单进程模式
    if (workers <= 1) {
        cf->workers = 1;
        return zv_worker_run(cf, 0);
    }

    cf->workers = workers;
    // 多进程模式
    // 安装master进程信号处理函数
    if (zv_install_master_signals() != 0) {
        log_err("install master signals failed");
        return 1;
    }
    // 分配保存子进程PID的数组
    pid_t *pids = (pid_t *)calloc((size_t)workers, sizeof(pid_t));
    if (!pids) {
        log_err("calloc(pids) failed");
        return 1;
    }
    log_status("zaver master starting. workers=%d pid=%d", workers, getpid());
    // 创建worker子进程
    for (int i = 0; i < workers; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            log_err("fork failed");
            zv_stop = 1;
            break; 
        }
        if (pid == 0) {
            int rc = zv_worker_run(cf, i);
            _exit(rc);//子进程运行结束后退出 (这个退出不会刷新缓冲区 虽然在这里没什么影响)
        }
        pids[i] = pid;
    }
    // master进程等待子进程退出
    while (!zv_stop) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, 0);//阻塞等待任一子进程退出
        if (pid < 0) {
            if (errno == EINTR) continue;//被信号中断则继续等待
            log_err("waitpid failed");
            break;
        }
        // 某个子进程退出，记录日志并准备关闭服务器
        log_err("worker exited pid=%d status=%d; shutting down", (int)pid, status);
        zv_stop = 1;
        break;
    }
    // 终止所有子进程
    for (int i = 0; i < workers; i++) {
        if (pids[i] > 0) {
            kill(pids[i], SIGTERM);//发送终止信号
        }
    }
    // 等待所有子进程退出
    for (int i = 0; i < workers; i++) {
        if (pids[i] > 0) {
            (void)waitpid(pids[i], NULL, 0);//阻塞等待指定子进程退出
        }
    }
    // 释放资源并退出
    free(pids);
    log_info("zaver master stopped");
    return 0;
}
