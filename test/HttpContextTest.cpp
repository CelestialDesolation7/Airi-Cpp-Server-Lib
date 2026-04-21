#include "http/HttpContext.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

[[noreturn]] void fail(const std::string &msg) {
    std::cerr << "[HttpContextTest] 失败: " << msg << "\n";
    std::exit(1);
}

void check(bool cond, const std::string &msg) {
    if (!cond)
        fail(msg);
}

void testPipelineConsumedBytes() {
    std::cout << "[HttpContextTest] 用例1：同包双请求（pipeline）消费字节验证\n";

    const std::string reqA = "GET /a HTTP/1.1\r\n"
                             "Host: local\r\n"
                             "\r\n";
    const std::string reqB = "GET /b HTTP/1.1\r\n"
                             "Host: local\r\n"
                             "\r\n";
    const std::string merged = reqA + reqB;

    HttpContext ctx;
    int consumedA = 0;
    bool ok = ctx.parse(merged.data(), static_cast<int>(merged.size()), &consumedA);

    check(ok, "解析失败（第一条请求）");
    check(ctx.isComplete(), "第一条请求应已完整");
    check(ctx.request().url() == "/a", "第一条 URL 应为 /a");
    check(consumedA == static_cast<int>(reqA.size()), "第一条消费字节应精确等于 reqA 长度");

    ctx.reset();
    int consumedB = 0;
    ok = ctx.parse(merged.data() + consumedA, static_cast<int>(merged.size()) - consumedA,
                   &consumedB);

    check(ok, "解析失败（第二条请求）");
    check(ctx.isComplete(), "第二条请求应已完整");
    check(ctx.request().url() == "/b", "第二条 URL 应为 /b");
    check(consumedB == static_cast<int>(reqB.size()), "第二条消费字节应精确等于 reqB 长度");
}

void testBodyFragmentAndTailRequest() {
    std::cout << "[HttpContextTest] 用例2：分段 body + 尾随下一请求验证\n";

    HttpContext ctx;

    const std::string part1 = "POST /echo HTTP/1.1\r\n"
                              "Host: local\r\n"
                              "Content-Length: 11\r\n"
                              "\r\n"
                              "hello";

    const std::string part2 = " world"
                              "GET /next HTTP/1.1\r\n"
                              "Host: local\r\n"
                              "\r\n";

    int consumed1 = 0;
    bool ok = ctx.parse(part1.data(), static_cast<int>(part1.size()), &consumed1);
    check(ok, "part1 解析失败");
    check(!ctx.isComplete(), "part1 后请求不应完整");
    check(consumed1 == static_cast<int>(part1.size()), "part1 应全部被消费");

    int consumed2 = 0;
    ok = ctx.parse(part2.data(), static_cast<int>(part2.size()), &consumed2);
    check(ok, "part2 解析失败");
    check(ctx.isComplete(), "补齐 body 后请求应完整");
    check(ctx.request().body() == "hello world", "POST body 应为 hello world");
    check(consumed2 == 6, "part2 中本次应只消费剩余 body 的 6 字节");

    ctx.reset();
    const char *tailReq = part2.data() + consumed2;
    const int tailLen = static_cast<int>(part2.size()) - consumed2;
    int consumed3 = 0;
    ok = ctx.parse(tailReq, tailLen, &consumed3);

    check(ok, "尾随请求解析失败");
    check(ctx.isComplete(), "尾随请求应完整");
    check(ctx.request().url() == "/next", "尾随请求 URL 应为 /next");
    check(consumed3 == tailLen, "尾随请求应完整消费 tail 数据");
}

void testInvalidMethod() {
    std::cout << "[HttpContextTest] 用例3：非法方法报文检测\n";

    HttpContext ctx;
    const std::string badReq = "BAD / HTTP/1.1\r\n"
                               "Host: local\r\n"
                               "\r\n";

    int consumed = 0;
    bool ok = ctx.parse(badReq.data(), static_cast<int>(badReq.size()), &consumed);
    check(!ok, "非法方法请求应解析失败");
    check(ctx.isInvalid(), "上下文状态应为 Invalid");
    check(consumed > 0, "非法请求也应推进解析位置，避免上层死循环");
}

} // namespace

int main() {
    std::cout << "[HttpContextTest] 开始执行\n";

    testPipelineConsumedBytes();
    testBodyFragmentAndTailRequest();
    testInvalidMethod();

    std::cout << "[HttpContextTest] 全部通过\n";
    return 0;
}
