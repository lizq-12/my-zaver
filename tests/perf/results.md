# Zaver Performance Results

- Time: 2026-01-13 06:42:31 UTC
- Binary: ./build/zaver
- Config: /home/lizq/lizqpi/network_code/zaver/zaver.conf
- Threads: 4
- Conns: 500
- Duration: 30s (warmup 3s)

| Case | URL | Threads | Conns | Duration | Requests/sec | Latency(avg) | Transfer/sec |
|---|---|---:|---:|---:|---:|---:|---:|
| Static small | http://127.0.0.1:3000/index.html | 4 | 500 | 30s | 54647.45 | 15.99ms | 38.41MB |
| Static big (256MiB) | http://127.0.0.1:3000/big.bin | 4 | 500 | 30s | 18.07 | 0.00us | 6.99GB |
| CGI | http://127.0.0.1:3000/cgi-bin/hello.sh | 4 | 500 | 30s | 83.07 | 0.00us | 8.66KB |

> Notes
> - This benchmark assumes a single zaver instance owns the port.
> - For stable results, run on an idle machine and repeat 3 times.
