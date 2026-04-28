// src/common/include/openems/common/service_utils.h
#pragma once

#include <functional>

namespace openems::common {

// 设置 SIGINT/SIGTERM 信号处理，执行 fn 直到收到信号后退出
// fn 接收一个 atomic<bool> 引用作为运行标志
void run_until_signal(std::function<void()> fn);

} // namespace openems::common