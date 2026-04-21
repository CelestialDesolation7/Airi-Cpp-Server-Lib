#include "InetAddress.h"
#include "Socket.h"

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

[[noreturn]] void fail(const std::string &msg) {
    std::cerr << "[SocketPolicyTest] 失败: " << msg << "\n";
    std::exit(1);
}

void check(bool cond, const std::string &msg) {
    if (!cond)
        fail(msg);
}

void testInvalidFdPolicy() {
    std::cout << "[SocketPolicyTest] 用例1：无效 fd 的返回值语义\n";

    Socket invalid(-1);
    InetAddress addr("127.0.0.1", 1);

    check(!invalid.bind(&addr), "无效 fd 上 bind 应返回 false");
    check(errno == EBADF, "无效 fd 的 bind 应设置 errno=EBADF");

    check(!invalid.listen(), "无效 fd 上 listen 应返回 false");
    check(!invalid.connect(&addr), "无效 fd 上 connect 应返回 false");
    check(invalid.accept(&addr) == -1, "无效 fd 上 accept 应返回 -1");
    check(!invalid.setnonblocking(), "无效 fd 上 setnonblocking 应返回 false");
}

void testValidSocketPolicy() {
    std::cout << "[SocketPolicyTest] 用例2：有效 fd 的 bind/listen/nonblock\n";

    Socket sock;
    check(sock.isValid(), "socket() 成功时 fd 应有效");

    check(sock.setnonblocking(), "有效 fd 设置非阻塞应成功");
    check(sock.isNonBlocking(), "设置非阻塞后 isNonBlocking 应为 true");

    InetAddress any("127.0.0.1", 0);
    check(sock.bind(&any), "绑定 127.0.0.1:0（临时端口）应成功");
    check(sock.listen(), "listen 应成功");
}

} // namespace

int main() {
    std::cout << "[SocketPolicyTest] 开始执行\n";

    testInvalidFdPolicy();
    testValidSocketPolicy();

    std::cout << "[SocketPolicyTest] 全部通过\n";
    return 0;
}
