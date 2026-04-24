#include "Connection.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

[[noreturn]] void fail(const std::string &msg) {
    std::cerr << "[BackpressureDecisionTest] 失败: " << msg << "\n";
    std::exit(1);
}

void check(bool cond, const std::string &msg) {
    if (!cond)
        fail(msg);
}

void testConfigValidation() {
    std::cout << "[BackpressureDecisionTest] 用例1：配置合法性校验\n";

    Connection::BackpressureConfig good{};
    check(Connection::isValidBackpressureConfig(good), "默认配置应合法");

    Connection::BackpressureConfig bad1{};
    bad1.lowWatermarkBytes = 0;
    check(!Connection::isValidBackpressureConfig(bad1), "low=0 应非法");

    Connection::BackpressureConfig bad2{};
    bad2.lowWatermarkBytes = 8;
    bad2.highWatermarkBytes = 4;
    check(!Connection::isValidBackpressureConfig(bad2), "low >= high 应非法");

    Connection::BackpressureConfig bad3{};
    bad3.lowWatermarkBytes = 4;
    bad3.highWatermarkBytes = 8;
    bad3.hardLimitBytes = 8;
    check(!Connection::isValidBackpressureConfig(bad3), "high >= hardLimit 应非法");
}

void testPauseResumeDecision() {
    std::cout << "[BackpressureDecisionTest] 用例2：暂停/恢复读事件决策\n";

    Connection::BackpressureConfig cfg{};
    cfg.lowWatermarkBytes = 10;
    cfg.highWatermarkBytes = 20;
    cfg.hardLimitBytes = 30;

    auto d1 = Connection::evaluateBackpressure(5, false, cfg);
    check(!d1.shouldPauseRead && !d1.shouldResumeRead && !d1.shouldCloseConnection,
          "buffer=5 不应触发动作");

    auto d2 = Connection::evaluateBackpressure(21, false, cfg);
    check(d2.shouldPauseRead && !d2.shouldResumeRead && !d2.shouldCloseConnection,
          "buffer=21（未暂停）应触发暂停读");

    auto d3 = Connection::evaluateBackpressure(15, true, cfg);
    check(!d3.shouldPauseRead && !d3.shouldResumeRead && !d3.shouldCloseConnection,
          "buffer=15（已暂停）不应立即恢复");

    auto d4 = Connection::evaluateBackpressure(10, true, cfg);
    check(!d4.shouldPauseRead && d4.shouldResumeRead && !d4.shouldCloseConnection,
          "buffer=10（已暂停）应恢复读");
}

void testHardLimitDecision() {
    std::cout << "[BackpressureDecisionTest] 用例3：硬上限保护决策\n";

    Connection::BackpressureConfig cfg{};
    cfg.lowWatermarkBytes = 10;
    cfg.highWatermarkBytes = 20;
    cfg.hardLimitBytes = 30;

    auto d = Connection::evaluateBackpressure(31, false, cfg);
    check(d.shouldCloseConnection, "buffer>hardLimit 应触发保护性断连");
}

} // namespace

int main() {
    std::cout << "[BackpressureDecisionTest] 开始执行\n";

    testConfigValidation();
    testPauseResumeDecision();
    testHardLimitDecision();

    std::cout << "[BackpressureDecisionTest] 全部通过\n";
    return 0;
}
