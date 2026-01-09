Zaver
=====

Yet another fast and efficient HTTP server.

## purpose

The purpose of Zaver is to help developers understand how to write a high performance server based on epoll. Although Nginx is a good learning example, its complexity and huge code base make people discouraged. Zaver uses as few codes as possible to demonstrate the core structure of high performance server like Nginx. Developers can lay a solid foundation by learning Zaver for further study in network programming.

## programming model

* epoll
* non-blocking I/O
* thread-pool

## compile and run (for now only support Linux2.6+)

please make sure you have [cmake](https://cmake.org/) installed.
```
mkdir build && cd build
cmake .. && make
cd .. && ./build/zaver -c zaver.conf
```

## support

* HTTP persistent connection
* browser cache
* timer(use binary heap instead of rbtree used in Nginx)

## todo

* sendfile (done)
* proxy
* FastCGI
* other HTTP/1.1 features
* memory pool
* WebDAV?

## more details

https://zyearn.github.io/blog/2015/05/16/how-to-write-a-server/

## Config

```
root=./html
port=3000
threadnum=4
workers=0
cpu_affinity=0
```

- `workers`: worker 进程数。
	- `0` 或负数：自动按 CPU 核数启动（每核一个 worker）
	- `1`：单进程
	- `>1`：多进程（使用 `SO_REUSEPORT`，每个 worker 各自 epoll loop）

- `cpu_affinity`: 是否为 worker 绑定 CPU 亲和性。
	- `0`：关闭
	- `1`：开启（worker 按 `worker_id` 轮转绑定到可用 CPU；master 不绑定，因为几乎只负责 wait 回收，CPU 占用很低）

