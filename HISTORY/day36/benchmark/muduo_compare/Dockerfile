# Dockerfile — Airi-Cpp-Server-Lib vs muduo 真实横向基准测试环境
#
# 在容器内：
#   1. 从源码构建 muduo（Linux-only，本身需要 boost）
#   2. 构建 muduo_bench_server（基于 muduo::net::HttpServer）
#   3. 构建 Airi-Cpp-Server-Lib + bench_server（CMake target，基于本项目 HttpServer）
#   4. 安装 wrk（业界标准 HTTP benchmark 工具）
#
# 用法：见同目录 README.md
#
# 必须从仓库根目录构建：
#   docker build -f benchmark/muduo_compare/Dockerfile -t mcpp-bench:latest .

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=UTC

# ── 系统依赖 ─────────────────────────────────────────────────────────
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    git \
    ca-certificates \
    libboost-all-dev \
    libssl-dev \
    zlib1g-dev \
    wrk \
    curl \
    procps \
    && rm -rf /var/lib/apt/lists/*

# ── 1. 构建 muduo ────────────────────────────────────────────────────
WORKDIR /opt
RUN git clone --depth=1 https://github.com/chenshuo/muduo.git muduo \
    && cd muduo \
    && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j $(nproc) \
    && cmake --install build --prefix /usr/local

# ── 2. 拷贝并构建本项目（包含 bench_server target）──────────────────
WORKDIR /workspace
COPY CMakeLists.txt /workspace/
COPY cmake/ /workspace/cmake/
COPY src/ /workspace/src/
COPY demo/ /workspace/demo/
COPY benchmark/ /workspace/benchmark/

RUN cmake -S /workspace -B /workspace/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DMCPP_ENABLE_TESTING=OFF \
    && cmake --build /workspace/build -j $(nproc) --target bench_server demo_server \
    && cp /workspace/build/bench_server /usr/local/bin/bench_server

# ── 3. 构建 muduo_bench_server ───────────────────────────────────────
RUN g++ -O2 -std=c++14 \
        /workspace/benchmark/muduo_compare/muduo_bench_server.cc \
        -lmuduo_http -lmuduo_net -lmuduo_base -lpthread \
        -o /usr/local/bin/muduo_bench_server

# ── 4. 安装运行脚本 ──────────────────────────────────────────────────
RUN cp /workspace/benchmark/muduo_compare/run_bench.sh /usr/local/bin/run_bench.sh \
    && chmod +x /usr/local/bin/run_bench.sh

WORKDIR /work
CMD ["/usr/local/bin/run_bench.sh"]
