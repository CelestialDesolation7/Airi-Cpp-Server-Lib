# Airi-Cpp-Server-Lib vs muduo 真实横向基准测试

> 这是 day36 的**真实**对照基准测试。muduo 是 Linux-only（依赖 epoll），所以本测试统一在 Docker 容器（Linux）内运行：
> - 同一镜像、同一 CPU、同一编译器、同一 wrk 配置
> - 服务端二者**响应体完全相同**（6 字节 `hello\n`）
> - 服务端二者**线程数完全相同**（默认 IO threads = 4）
> - 客户端使用业界标准 [`wrk`](https://github.com/wg/wrk)，不是自研客户端

## 文件清单

| 文件 | 作用 |
|------|------|
| `Dockerfile` | 构建包含 muduo + 本项目 + wrk 的 Linux 测试镜像 |
| `bench_server.cpp` | 本项目的 HTTP 基准服务器（无中间件） |
| `muduo_bench_server.cc` | muduo 的对照服务器（同响应、同线程） |
| `run_bench.sh` | 容器内执行：依次启动两服务器，wrk 压测，生成 `summary.md` |
| `results/` | 跑完后输出的原始 wrk 报告 + summary |

## 一键运行

要求：宿主机已安装 Docker（macOS / Linux 均可）。

```bash
# 1. 在仓库根目录构建镜像（耗时 5-10 分钟，主要花在编译 muduo）
cd /path/to/Airi-Cpp-Server-Lib
docker build -f benchmark/muduo_compare/Dockerfile -t mcpp-bench:latest .

# 2. 跑测试，结果挂载到本机 benchmark/muduo_compare/results
mkdir -p benchmark/muduo_compare/results
docker run --rm \
    -v "$PWD/benchmark/muduo_compare/results:/work/results" \
    mcpp-bench:latest

# 3. 查看 summary
cat benchmark/muduo_compare/results/summary.md
```

## 自定义参数

通过环境变量调整：

```bash
docker run --rm \
    -e IO_THREADS=8 \
    -e WRK_THREADS=8 \
    -e DURATION=60s \
    -v "$PWD/benchmark/muduo_compare/results:/work/results" \
    mcpp-bench:latest
```

## 测试矩阵

| 用例 | wrk 线程 | 连接数 | 时长 | 备注 |
|------|---------|-------|------|------|
| c100 | 4 | 100 | 30s | 中并发 keep-alive |
| c1000 | 4 | 1000 | 30s | 高并发 keep-alive |

每个用例对两个服务器各跑一次，共 4 轮。

## 公平性约束

- 两服务器的响应体一字不差（`"hello\n"`）
- 关闭日志（本项目设 WARN，muduo 默认）
- 同样的 IO 线程数
- 同 CPU、同内存、同 kernel（容器隔离）
- 客户端、服务端同机（避免网卡与丢包噪声）
- 连接复用（HTTP/1.1 keep-alive，wrk 默认行为）

**Caveat**：本机 Docker 是虚拟机封装，性能本身比裸金属低 10%-30%。横向对比仍有效，但**绝对数值不应当作生产数据使用**。

## 复现近期结果

最近一次测试结果见仓库 `dev-log/day36.md` "测试与验证" 章节，含原始 wrk 输出与对比表格。
