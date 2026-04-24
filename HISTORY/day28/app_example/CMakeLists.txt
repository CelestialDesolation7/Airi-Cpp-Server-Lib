cmake_minimum_required(VERSION 3.10)
project(AppExample LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─────────────────────────────────────────────────────────────────────────
# Day 28 引入 app_example 的目的：演示"应用层如何作为独立 CMake 子项目，
# 复用上一层目录中的网络库头文件与源代码"。
#
# 设计选择：直接以相对路径 include 父目录的 include/ 与 common/，避免
# 在 external/ 下保留任何头文件副本（那会让 git diff 在每次库头改动时
# 都重复记录两份变更，污染历史）。
# ─────────────────────────────────────────────────────────────────────────

set(LIB_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# 网络库源码：与父项目 CMakeLists.txt 中 server 目标使用的列表保持一致
set(LIB_POLLER_SRCS ${LIB_ROOT}/common/Poller/DefaultPoller.cpp)
if(APPLE)
    list(APPEND LIB_POLLER_SRCS ${LIB_ROOT}/common/Poller/kqueue/KqueuePoller.cpp)
else()
    list(APPEND LIB_POLLER_SRCS ${LIB_ROOT}/common/Poller/epoll/EpollPoller.cpp)
endif()

set(LIB_SRCS
    ${LIB_ROOT}/common/Acceptor.cpp
    ${LIB_ROOT}/common/Buffer.cpp
    ${LIB_ROOT}/common/Channel.cpp
    ${LIB_ROOT}/common/Connection.cpp
    ${LIB_ROOT}/common/EventLoopThread.cpp
    ${LIB_ROOT}/common/EventLoopThreadPool.cpp
    ${LIB_ROOT}/common/Eventloop.cpp
    ${LIB_ROOT}/common/InetAddress.cpp
    ${LIB_ROOT}/common/Socket.cpp
    ${LIB_ROOT}/common/TcpServer.cpp
    ${LIB_ROOT}/common/ThreadPool.cpp
    ${LIB_ROOT}/common/timer/TimerQueue.cpp
    ${LIB_ROOT}/common/log/AsyncLogging.cpp
    ${LIB_ROOT}/common/log/LogFile.cpp
    ${LIB_ROOT}/common/log/LogStream.cpp
    ${LIB_ROOT}/common/log/Logger.cpp
    ${LIB_ROOT}/common/http/HttpContext.cpp
    ${LIB_ROOT}/common/http/HttpRequest.cpp
    ${LIB_ROOT}/common/http/HttpResponse.cpp
    ${LIB_ROOT}/common/http/HttpServer.cpp
    ${LIB_POLLER_SRCS}
)

add_executable(http_server src/http_server.cpp ${LIB_SRCS})
target_include_directories(http_server PRIVATE ${LIB_ROOT}/include)
target_link_libraries(http_server PRIVATE pthread)

# 运行期需要的静态资源：拷贝到构建目录
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/static DESTINATION ${CMAKE_BINARY_DIR})
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/files DESTINATION ${CMAKE_BINARY_DIR})
