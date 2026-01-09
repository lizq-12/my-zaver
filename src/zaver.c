#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include "dbg.h"
#include "process.h"
#include "util.h"

#define CONF "zaver.conf"
#define PROGRAM_VERSION "0.1"
// 
static const struct option long_options[]=
{
    {"help",no_argument,NULL,'?'},
    {"version",no_argument,NULL,'V'},
    {"conf",required_argument,NULL,'c'},
    {NULL,0,NULL,0}
};
//打印程序使用信息
static void usage() {
   fprintf(stderr,
	"zaver [option]... \n"
	"  -c|--conf <config file>  Specify config file. Default ./zaver.conf.\n"
	"  -?|-h|--help             This information.\n"
	"  -V|--version             Display program version.\n"
	);
}
//程序入口函数
int main(int argc, char* argv[]) 
{
    int rc;
    int opt = 0;
    int options_index = 0;
    char *conf_file = CONF;
    //如果没有任何参数则打印使用信息
    if (argc == 1) {
        usage();
        return 0;
    }
    //解析命令行参数
    while ((opt=getopt_long(argc, argv,"Vc:?h",long_options,&options_index)) != EOF) {
        switch (opt) {
            case  0 : break;
            case 'c':
                conf_file = optarg;
                break;
            case 'V':
                printf(PROGRAM_VERSION"\n");
                return 0;
            case ':':
            case 'h':
            case '?':
                usage();
                return 0;
        }
    }
    //打印配置文件路径
    debug("conffile = %s", conf_file);
    //检查是否有非选项参数
    if (optind < argc) {
        log_err("non-option ARGV-elements: ");
        while (optind < argc)
            log_err("%s ", argv[optind++]);
        return 0;
    }
    //读取配置文件 填充配置结构体cf
    char conf_buf[BUFLEN];
    zv_conf_t cf;
    rc = read_conf(conf_file, &cf, conf_buf, BUFLEN);
    if (rc != ZV_CONF_OK) {
        log_err("read conf err: %s", conf_file);
        return 1;
    }

    log_status("zaver started. port=%d workers=%d cpu_affinity=%d", cf.port, cf.workers,cf.cpu_affinity);
    //运行服务器
    return zv_run_server(&cf);
}
