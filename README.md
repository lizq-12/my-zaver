Zaver
=====

**Zaver** is a lightweight, high-performance HTTP web server written in C for Linux.

## purpose

The purpose of Zaver is to help developers understand how to write a high-performance server based on **Epoll** and **Linux System Programming**.

The purpose of Zaver is to help developers understand how to write a high performance server based on epoll. Although Nginx is a good learning example, its complexity and huge code base make people discouraged. Zaver uses as few codes as possible to demonstrate the core structure of high performance server like Nginx. Developers can lay a solid foundation by learning Zaver for further study in network programming.

## programming model

Zaver has evolved from a simple thread model to a robust **Multi-Reactor Process Model**, designed to fully utilize multi-core processors.

* **Multi-Process Architecture**: Uses a Master-Worker model with **SO_REUSEPORT** for kernel-level load balancing, eliminating the "Thundering Herd" problem.
* **Event Driven**: Based on **Epoll (Edge Triggered)** and Non-blocking I/O.
* **Concurrency**: Capable of handling tens of thousands of concurrent connections with low memory footprint.

## Key Features & Optimizations

* **⚡ Extreme Performance**:
    * **Zero-Copy**: Implemented `sendfile` to minimize user-kernel mode context switching and CPU data copying, achieving **8GB/s+ throughput** for large files.
    * **CPU Affinity**: Supports binding worker processes to specific CPU cores to reduce cache thrashing and maximize L1/L2 cache hit rates.
* **🧠 Memory Management**:
    * **Object Pool**: Custom allocator (Free List) for HTTP request objects to eliminate frequent `malloc/free` overhead and reduce memory fragmentation.
    * **Memory Pool**: Region-based memory management for temporary data parsing.
* **🛡️ Reliability & Security**:
    * **Path Sanitization**: robust protection against Path Traversal attacks (e.g., `../../etc/passwd`).
    * **CI/CD**: Integrated **GitHub Actions** for automated building and functional testing.
    * **Sanitizers**: Code is tested with **AddressSanitizer (ASan)** and **UndefinedBehaviorSanitizer (UBSan)** to ensure memory safety.

### Prerequisites
* Linux Kernel 3.9 or later (for `SO_REUSEPORT`)
* CMake & GCC/Clang

## compile and run

please make sure you have [cmake](https://cmake.org/) installed.
```
mkdir build && cd build
cmake .. && make
cd .. && ./build/zaver -c zaver.conf
```

## tests

Functional + security regression:
```bash
chmod +x tests/functional_test.sh
./tests/functional_test.sh
```

## performance benchmark

Prerequisite: install `wrk`.

Run a small suite (static small, static big, CGI) and write a Markdown summary to `tests/perf/results.md`:
```bash
chmod +x tests/perf/bench.sh
BUILD_DIR=build THREADS=4 CONNS=500 DURATION=30s ./tests/perf/bench.sh
```

## support

* HTTP/1.1 Persistent Connections (Keep-Alive)
* Static File Serving (Zero-Copy)
* Timer Management (Priority Queue / Min-Heap)
* Browser Cache Control
* Graceful Error Handling

## todo
* proxy
* FastCGI
* other HTTP/1.1 features
* WebDAV

## Config

```
root=./html
port=3000
threadnum=4
workers=0
cpu_affinity=0
keep_alive_timeout_ms=5000
request_timeout_ms=5000
```


