# Zaver Performance Results

- Time: 2026-01-14 08:09:57 UTC
- Binary: ./build/zaver
- Config: /home/lizq/lizqpi/network_code/zaver/zaver.conf
- Mode: claims
- Threads: 4
- Base Conns: 500
- Duration: 30s (warmup 3s)
- Runs per case: 1
- wrk timeout: 10s
- workers (from conf): 4

## C10K / High Concurrency (Static small)

| Case | URL | Workers | Threads | Conns | Duration | Runs | Requests/sec(mean) | Lat(avg) | Lat(stdev) | Lat(max) | p50 | p90 | p99 | Transfer/sec(mean) | Non2xx(sum) | Sockerr | Notes |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| Static small | http://127.0.0.1:3000/index.html | 4 | 4 | 1000 | 30s | 1 | 63475.450000 | 19.120ms | 27.270ms | 371.270ms | 15.200ms | 27.640ms | 166.620ms | 44.61MB | 0 | 0 | - |
| Static small | http://127.0.0.1:3000/index.html | 4 | 4 | 2000 | 30s | 1 | 59872.390000 | 37.050ms | 38.840ms | 662.060ms | 34.470ms | 53.020ms | 222.020ms | 42.08MB | 0 | 0 | - |
| Static small | http://127.0.0.1:3000/index.html | 4 | 4 | 5000 | 30s | 1 | 56770.320000 | 98.850ms | 103.050ms | 1230.000ms | 93.080ms | 131.970ms | 675.610ms | 39.90MB | 0 | 0 | - |
| Static small | http://127.0.0.1:3000/index.html | 4 | 4 | 10000 | 30s | 1 | 59752.110000 | 173.490ms | 122.190ms | 1380.000ms | 179.200ms | 227.690ms | 790.790ms | 42.00MB | 0 | 0 | - |

### Idle Keep-Alive Memory (best-effort)

| Metric | Target conns | Opened conns | Hold | Result |
|---|---:|---:|---:|---|
| Idle keep-alive RSS (proc RSS only) | 10000 | 10000 | 10s | rss_kb_before=324864; rss_kb_after=324864; delta_kb=0; bytes_per_conn=0.0 |


## Single-Core QPS (Static small, workers=1)

| Case | URL | Workers | Threads | Conns | Duration | Runs | Requests/sec(mean) | Lat(avg) | Lat(stdev) | Lat(max) | p50 | p90 | p99 | Transfer/sec(mean) | Non2xx(sum) | Sockerr | Notes |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| Static small (single-core config) | http://127.0.0.1:3000/index.html | 1 | 1 | 2000 | 30s | 1 | 22448.600000 | 88.260ms | 31.290ms | 203.900ms | 84.710ms | 130.020ms | 178.170ms | 15.78MB | 0 | 0 | - |


> Notes
> - This benchmark assumes a single zaver instance owns the port (SO_REUSEPORT allows multiple instances).
> - For stable results, run on an idle machine and set RUNS=3 (or higher).
> - 
> Nginx-style claims are environment-dependent:
> - 
>   - C10K/QPS depend on CPU/OS/tuning and client limits (ulimit).
>   - Idle keep-alive memory here measures process RSS only (kernel socket memory is not included).
