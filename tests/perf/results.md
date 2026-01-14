# Zaver Performance Results

- Time: 2026-01-14 08:08:21 UTC
- Binary: ./build/zaver
- Config: /home/lizq/lizqpi/network_code/zaver/zaver.conf
- Mode: scan_conns
- Threads: 4
- Base Conns: 500
- Duration: 1s (warmup 3s)
- Runs per case: 1
- wrk timeout: 10s
- workers (from conf): 4

## Conns Scan (Static small)

| Case | URL | Workers | Threads | Conns | Duration | Runs | Requests/sec(mean) | Lat(avg) | Lat(stdev) | Lat(max) | p50 | p90 | p99 | Transfer/sec(mean) | Non2xx(sum) | Sockerr | Notes |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| Static small | http://127.0.0.1:3000/index.html | 4 | 4 | 50 | 1s | 1 | 51248.880000 | 2.200ms | 4.100ms | 38.980ms | 0.488ms | 5.250ms | 21.830ms | 36.02MB | 0 | 0 | - |
| Static small | http://127.0.0.1:3000/index.html | 4 | 4 | 100 | 1s | 1 | 59509.270000 | 2.030ms | 2.610ms | 26.640ms | 1.320ms | 4.470ms | 14.070ms | 41.83MB | 0 | 0 | - |
| Static small | http://127.0.0.1:3000/index.html | 4 | 4 | 200 | 1s | 1 | 64045.580000 | 4.030ms | 6.400ms | 70.920ms | 2.690ms | 6.450ms | 36.810ms | 45.01MB | 0 | 0 | - |
| Static small | http://127.0.0.1:3000/index.html | 4 | 4 | 500 | 1s | 1 | 69020.410000 | 7.490ms | 7.470ms | 79.180ms | 6.230ms | 12.470ms | 40.950ms | 48.51MB | 0 | 0 | - |
| Static small | http://127.0.0.1:3000/index.html | 4 | 4 | 1000 | 1s | 1 | 60847.060000 | 15.060ms | 8.430ms | 60.540ms | 16.650ms | 23.260ms | 37.200ms | 42.77MB | 0 | 0 | - |


> Notes
> - This benchmark assumes a single zaver instance owns the port (SO_REUSEPORT allows multiple instances).
> - For stable results, run on an idle machine and set RUNS=3 (or higher).
> - 
> Nginx-style claims are environment-dependent:
> - 
>   - C10K/QPS depend on CPU/OS/tuning and client limits (ulimit).
>   - Idle keep-alive memory here measures process RSS only (kernel socket memory is not included).
