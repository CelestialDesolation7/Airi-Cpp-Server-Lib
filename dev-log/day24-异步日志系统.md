# Day 24 — 异步日志系统（双缓冲 AsyncLogging）

> **主题**：构建生产级异步日志框架——同步前端 + 异步后端 + 双缓冲区交换。  
> **基于**：Day 23（跨平台定时器 TimerQueue）

---

## 1. 引言

### 1.1 问题上下文

到 Day 23，所有日志仍然是同步 `std::cout` / `printf`——每条日志都阻塞调用线程做 IO。压测时这是隐形杀手：业务线程被磁盘 IO 拖慢，QPS 下降可能 10×。

工业级 C++ 服务器的标配是**异步日志**：前端线程只把日志格式化到内存缓冲区（纳秒级开销），后端线程批量写盘。muduo 的双缓冲（current / next）设计是这个领域的教科书方案——前端用 mutex 短暂保护一次"指针交换"，后端拿走满 buffer 慢慢写，零拷贝、低延迟。

### 1.2 动机

同步日志的开销从"每条 write 系统调用"上升为"`fwrite` 缓冲 + 偶尔 flush"，业务线程几乎无感。但只要有 IO 在业务路径上，尾延迟就不可控（磁盘抖动 / fsync 卡顿）。

异步日志彻底解耦——业务路径只动内存，磁盘 IO 在专用线程，业务延迟不再受日志影响。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| `std::cout` / `printf` 同步 | 教学 | 简单 | 阻塞业务、IO 抖动放大延迟 |
| 异步日志 + 双缓冲 (本日) | muduo `AsyncLogging` | 业务零阻塞、批量 flush | 实现复杂、断电可能丢日志 |
| spdlog | spdlog (现代 C++) | 易用、高性能、丰富 sink | 引入大型依赖 |
| log4cxx / glog | 老牌 | 功能完整 | API 旧 |
| Rust `tracing` + appender | Rust | 结构化、零开销 | 跨语言 |
| OpenTelemetry logs | OTel | 可观测性标准 | 需 collector |

### 1.4 本日方案概述

本日实现：
1. `LogStream.h/cpp`：`FixedBuffer<N>` 固定缓冲区 + `LogStream` 流式 `operator<<`（int / double / string / pointer ...）。
2. `Logger.h/cpp`：同步前端 `Logger`，`LOG_INFO` / `LOG_WARN` / `LOG_ERROR` / `LOG_FATAL` 宏；析构时调全局 `g_output(buf, len)`。
3. `LogFile.h/cpp`：日志文件写入器，按时间/大小自动滚动，文件名带时间戳。
4. `AsyncLogging.h/cpp`：双缓冲后端。前端 `append`：当前 buffer 未满 → 直接写；满了 → 推入 `buffers_` 队列、换 next、`cv_.notify` 后端。后端线程：`cv_.wait` 拿到一批满 buffer → `flush` 到 `LogFile` → 把空 buffer 还给前端。
5. `Latch.h`（CountdownLatch）：线程间启动门闩。
6. 在 `main()` 启动 AsyncLogging 后调 `Logger::setOutput(AsyncLogging::append)`，把全局输出函数替换。
7. 新增 `test/LogTest.cpp`：同步、格式化、异步多线程压测。

下一天用这套日志框架支撑 HTTP 协议层的请求日志输出。

---
## 2. 文件变更总览

| 文件 | 状态 | 说明 |
|------|------|------|
| `include/log/LogStream.h` | **新增** | `FixedBuffer<N>` 固定缓冲区 + `LogStream` 流式格式化（operator<<） |
| `common/log/LogStream.cpp` | **新增** | LogStream 各类型 operator<< 实现 |
| `include/log/Logger.h` | **新增** | 同步日志前端：Logger 类 + LOG_* 宏 |
| `common/log/Logger.cpp` | **新增** | Logger 实现：时间戳 + 线程编号 + 级别 + 输出函数指针 |
| `include/log/LogFile.h` | **新增** | 日志文件写入器：自动滚动、时间戳命名 |
| `common/log/LogFile.cpp` | **新增** | LogFile 实现：fopen/fwrite/fclose + rollFile |
| `include/log/AsyncLogging.h` | **新增** | 异步日志后端：双缓冲区 + 后端写线程 |
| `common/log/AsyncLogging.cpp` | **新增** | AsyncLogging 实现：前端 append + 后端 threadFunc |
| `include/Latch.h` | **新增** | CountdownLatch：线程间启动同步门闩 |
| `test/LogTest.cpp` | **新增** | 日志系统测试：同步输出、格式化、多线程异步写入 |

---

## 3. 模块全景与所有权树

```
日志系统全景
├── LOG_* 宏（编译期展开）
│   └── Logger（临时对象，析构时输出）
│       ├── Impl（组装日志行）
│       │   └── LogStream（格式化）
│       │       └── FixedBuffer<4KB>（单条日志缓冲）
│       └── g_output（全局输出函数指针）
│           ├── 默认：fwrite(stdout)
│           └── 可替换：AsyncLogging::append
│
├── AsyncLogging（异步后端）
│   ├── current_  → FixedBuffer<4MB>  前端写入
│   ├── next_     → FixedBuffer<4MB>  备用缓冲
│   ├── buffers_  → vector<unique_ptr<FixedBuffer<4MB>>>  已满队列
│   ├── thread_   → 后端写线程
│   ├── Latch     → 启动同步
│   └── LogFile（磁盘写入器）
│       └── FILE* fp_  → basename.YYYYMMDD_HHMMSS.log
│
└── Fmt（格式化辅助类）
```

---

## 4. 全流程调用链

### 4.1 同步日志（默认模式，输出到 stdout）

```
LOG_INFO << "server started, port=" << 8080;
  │ 展开为：Logger(__FILE__, __LINE__, INFO).stream() << ...
  │
  ├─── Logger 构造 → Impl 构造
  │    └── LogStream << 时间戳 << " T" << tid << " INFO "
  │
  ├─── 用户 << 链式调用
  │    └── LogStream << "server started, port=" << 8080
  │
  └─── Logger 析构 → Impl::finish()
       ├── LogStream << " - " << file << ":" << line << "\n"
       └── g_output(buf.data(), buf.len())
           └── fwrite(stdout)
```

### 4.2 异步日志（替换 g_output → AsyncLogging::append）

```
设置阶段：
  AsyncLogging alog("server");
  alog.start();
    ├── 创建后端 thread_
    ├── Latch::wait() 等待后端就绪
    └── threadFunc() { LogFile 初始化; latch_.countDown(); }
  Logger::setOutput([&](data,len){ alog.append(data,len); });

运行阶段：
  LOG_INFO << "msg"
    │ Logger 析构 → g_output → alog.append(data, len)
    ▼
  AsyncLogging::append(data, len)    ← 前端（业务线程，持锁极短）
    ├── current_->avail() >= len?
    │   ├── YES → current_->append(data, len)  快速路径
    │   └── NO  → buffers_.push_back(move(current_))
    │             current_ = move(next_)  或  new Buffer
    │             current_->append(data, len)
    │             cv_.notify_one()  唤醒后端
    ▼
  AsyncLogging::threadFunc()         ← 后端（写线程）
    ├── cv_.wait_for(3s) 或被 notify
    ├── lock { swap current_/buffers_ → local }
    ├── unlock
    ├── for buf : localBuffers → output.append(buf->data(), buf->len())
    │                            └── LogFile::append → fwrite → rollFile
    └── 归还 spare1/spare2 缓冲区对象
```

---

## 5. 代码逐段解析

### 5.1 FixedBuffer — 零堆分配固定缓冲区

```cpp
template <int SIZE>
class FixedBuffer {
    char data_[SIZE];   // 栈或成员内存
    char *cur_;         // 当前写入位置
  public:
    void append(const char *buf, int len) {
        if (avail() > len) { memcpy(cur_, buf, len); cur_ += len; }
    }
    int avail() const { return static_cast<int>(end() - cur_); }
};
```

**两种规格**：

| 常量 | 大小 | 用途 |
|------|------|------|
| `kSmallBuffer` | 4 KB | 前端 LogStream：一条日志最大 4KB |
| `kLargeBuffer` | 4 MB | 后端 AsyncLogging：批量缓冲区 |

### 5.2 LogStream — 流式格式化

```cpp
class LogStream {
    Buffer buffer_;  // FixedBuffer<kSmallBuffer>
  public:
    LogStream &operator<<(int n)   { formatNum("%d", n);   return *this; }
    LogStream &operator<<(double n){ formatNum("%.6g", n); return *this; }
    LogStream &operator<<(const char *str) {
        buffer_.append(str, strlen(str)); return *this;
    }
    // ...
  private:
    template <typename T>
    void formatNum(const char *fmt, T value) {
        if (buffer_.avail() >= kMaxNumericSize)
            buffer_.add(snprintf(buffer_.current(), kMaxNumericSize, fmt, value));
    }
};
```

**设计原则**：

- 所有格式化在栈缓冲区完成，**不触发堆分配**。
- `snprintf` 直接写入 `buffer_.current()`，避免中间字符串。

### 5.3 Logger — 同步前端

```cpp
Logger::Impl::Impl(LogLevel level, const SourceFile &file, int line) {
    stream_ << TimeStamp::now().toString();
    stream_ << " T" << tl_tid << ' ';      // 线程顺序编号
    stream_ << kLevelStr[level] << ' ';     // "INFO ", "ERROR" 等
}
Logger::~Logger() {
    impl_.finish();
    g_output(buf.data(), buf.len());   // 整条日志一次性输出
    if (level == FATAL) { g_flush(); abort(); }
}
```

**日志行格式**：`2025-04-21 14:30:00.123456 T1 INFO  server started - main.cpp:42`

**LOG_DEBUG 零开销**：

```cpp
#define LOG_DEBUG if (Logger::logLevel() <= Logger::DEBUG) \
    Logger(__FILE__, __LINE__, Logger::DEBUG).stream()
```

当日志级别 > DEBUG 时，`if` 条件为 false，Logger 不构造，用户的 `<<` 表达式不求值。

### 5.4 AsyncLogging — 双缓冲异步后端

核心思想：**前端写内存（微秒级），后端写磁盘（毫秒级），两者通过缓冲区交换解耦。**

```
┌──────────────┐    swap     ┌──────────────┐
│ 前端线程(们)  │ ──────────→ │ 后端写线程    │
│              │  lock 极短   │              │
│ current_ ────┤             ├──→ LogFile    │
│ next_    ────┤             │    fwrite()   │
│ buffers_ ────┤             │              │
└──────────────┘             └──────────────┘
```

**前端 append()**：

```cpp
void AsyncLogging::append(const char *data, int len) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_->avail() >= len) {
        current_->append(data, len);   // 最常见：直接 append
    } else {
        buffers_.push_back(std::move(current_));
        current_ = next_ ? std::move(next_) : make_unique<Buffer>();
        current_->append(data, len);
        cv_.notify_one();              // 缓冲区满，唤醒后端
    }
}
```

**后端 threadFunc()**：

```cpp
void AsyncLogging::threadFunc() {
    while (running_) {
        {
            unique_lock lock(mutex_);
            cv_.wait_for(lock, 3s, [&]{ return !buffers_.empty(); });
            buffers_.push_back(std::move(current_));
            current_ = std::move(spare1);
            localBuffers.swap(buffers_);
        } // 锁释放
        for (auto &buf : localBuffers)
            output.append(buf->data(), buf->len());  // 无锁磁盘 IO
        // 归还 spare 缓冲区，避免频繁 new
    }
}
```

**防积压保护**：缓冲区超过 25 个（≈100MB）时丢弃旧数据。

### 5.5 LogFile — 自动滚动日志文件

```cpp
void LogFile::append(const char *data, int len) {
    if (writtenBytes_ + len > rollSizeBytes_) rollFile();
    writtenBytes_ += fwrite(data, 1, len, fp_);
}
void LogFile::rollFile() {
    fclose(fp_);
    fp_ = fopen(getFileName().c_str(), "ae");  // 'e' = O_CLOEXEC
}
```

文件名格式：`server.20250421_143000.log`

### 5.6 Latch — 启动同步

```cpp
class Latch {
    int count_;
    std::mutex mutex_;
    std::condition_variable cv_;
  public:
    void wait()      { unique_lock lk(mutex_); cv_.wait(lk, [&]{ return count_ <= 0; }); }
    void countDown() { unique_lock lk(mutex_); if (--count_ <= 0) cv_.notify_all(); }
};
```

确保 `AsyncLogging::start()` 返回时后端线程已完成 LogFile 初始化。

### 5.7 Fmt — printf 风格格式化辅助

```cpp
LOG_INFO << "ratio=" << Fmt("%.2f", ratio) << "%";
```

编译期 `static_assert` 确保只接受算术类型，运行时 `snprintf` 格式化到栈缓冲区。

---

### 5.8 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day24/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day24/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 职责划分表

| 类 | 单一职责 |
|----|----------|
| `FixedBuffer<N>` | 固定大小字节缓冲区（零堆分配） |
| `LogStream` | 流式格式化，积累一条日志的所有字段 |
| `Logger` | 同步前端：构造组装日志头，析构触发输出 |
| `LogFile` | 磁盘文件写入：fwrite + 自动滚动 |
| `AsyncLogging` | 异步后端：双缓冲区交换 + 后端写线程 |
| `Latch` | 线程间一次性启动同步 |
| `Fmt` | printf 风格格式化辅助 |

---

## 7. 性能分析

| 操作 | 开销 | 说明 |
|------|------|------|
| LOG_INFO << "msg" | ~200ns | 仅内存拷贝到 FixedBuffer，无系统调用 |
| append() 快速路径 | ~50ns 临界区 | lock_guard + memcpy |
| append() 慢路径 | ~1μs | 交换缓冲区 + notify |
| 后端 fwrite | ~10-100μs | 取决于 OS 缓存和磁盘速度 |

前端 append 持锁时间极短（仅 memcpy），不会阻塞业务线程的 IO 处理。

---

## 8. 局限与后续

| 当前局限 | 后续改进方向 |
|----------|------------|
| LogFile 无滚动计数（同秒内可能覆盖） | 添加自增序号或进程 PID 到文件名 |
| Logger 不支持条件日志（如按模块过滤） | 添加 category/tag 机制 |
| 无日志压缩 | 大日志场景可后台 gzip |
| FATAL 直接 abort()，无 coredump 配置 | 生产环境配合 ulimit 和信号处理 |
| **→ Day 25**：HTTP 协议层（请求解析 + 响应构建 + HttpServer） | |
