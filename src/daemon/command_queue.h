/**
 * @file daemon/command_queue.h
 * @brief 插件命令的排队与执行：入队只拷字节，fork 留给 worker 主循环
 * @details 插件在输入钩子里判出一条 `run` 动作，经 ds5Host::command 调到这里
 *          的 Post：只做一次定长 memcpy 入队，不 fork、不做 I/O、不分配。真正的
 *          执行在 Drain（worker 主循环）里：先回收上一轮退出的子进程，再把环排空，
 *          每条命令 fork 一次 `/bin/sh -c`，不等待、不查退出码。
 * @details 不负责：解释命令内容（原样交给 shell）、管命令的生命周期（不留回执）、
 *          保证命令在那个身份下能做什么（执行身份就是降权后的 worker）。
 * @note 层次：daemon。线程模型：Post 任何线程可调；Drain 只在 worker 主循环上，
 *       且早于 plugin::Stop 不会发生，所以卸载与排队不并发。
 * @see plugin/plugin_api.h —— 插件看到的那一侧契约。
 */

#pragma once

#include <cstddef>
#include <string>

namespace ds5::daemon::command_queue {

/// 环里的条数上限；满了丢最新一条。
inline constexpr std::size_t kCapacity = 8;

/// 同时在跑（已 fork、未回收）的命令上限；排空时已达上限的这一条丢弃。
inline constexpr std::size_t kLiveMax = 64;

/// 在起传输线程之前调用：记住命令的工作目录（降权用户的 HOME）。
void Start(std::string home);

/// 入队一条命令；任何线程可调。true = 已入队，false = 超长或环满。
bool Post(const char* text);

/// 回收退出的子进程，再把环排空；只在 worker 主循环上调。
void Drain();

#ifdef DS5DUMMY_TESTING
/// 单测用：清空环与 pid 表，让每条用例从同一状态开始。
void ResetForTesting();

/// 单测用：仍在跑、尚未被 Drain 回收的子进程数。
std::size_t LiveForTesting();
#endif

}  // namespace ds5::daemon::command_queue