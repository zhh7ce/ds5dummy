/**
 * @file debug.h
 * @brief 两个互相独立的编译期调试开关，以及统一的日志语句宏
 *
 * `DS5DUMMY_DEBUG` 控制诊断代码是否存在于二进制里：计数器、每次写的耗时、
 * 5 秒统计窗口。`DS5DUMMY_DEBUG_LOG` 控制调试日志语句是否编译。两者都由
 * CMake 按构建类型注入（Debug 为 1，其余为 0）；未被注入时此处兜底为 0，
 * 所以单独编译某个 TU 也不会意外带入诊断代码。
 *
 * 日志语句只走本头的两个宏，业务代码不直接调用 loguru 的 `LOG_F`：
 * - `DS5DUMMY_LOG(verbosity, ...)`：始终编译。verbosity 写 loguru 的等级名
 *   （`INFO`/`WARNING`/`ERROR`）或数字；等级过滤是运行时的，打包服务用
 *   `-v WARNING` 决定 stderr 上看到哪些。
 * - `DS5DUMMY_DLOG(verbosity, ...)`：仅 `DS5DUMMY_DEBUG_LOG` 为 1 时编译，
 *   关闭时参数表达式整体消失、不会被求值。
 *
 * 整段诊断代码连同它的声明一起用 `#if DS5DUMMY_DEBUG` 包住，关闭时连符号
 * 都不存在，不留"永远返回零"的空壳；需要先构造日志负载的整段日志代码用
 * `#if DS5DUMMY_DEBUG_LOG` 包住。
 *
 * 语句宏不直接叫 `DS5DUMMY_DEBUG_LOG`：CMake 注入的是对象式宏，同名再
 * 定义成函数式宏属于重定义。`DLOG` 沿用 loguru/glog 的命名习惯。
 *
 * @note 使用这两个日志宏的文件必须先自行包含 loguru.hpp：宏展开直接产出
 *       `LOG_F`，头文件包含顺序在这里不被本头保证。
 */

#pragma once

#ifndef DS5DUMMY_DEBUG
#define DS5DUMMY_DEBUG 0
#endif

#ifndef DS5DUMMY_DEBUG_LOG
#define DS5DUMMY_DEBUG_LOG 0
#endif

#define DS5DUMMY_LOG(verbosity, ...) LOG_F(verbosity, __VA_ARGS__)

#if DS5DUMMY_DEBUG_LOG
#define DS5DUMMY_DLOG(verbosity, ...) DS5DUMMY_LOG(verbosity, __VA_ARGS__)
#else
// 空语句包成 do-while：保持宏在 if/for 单语句位置语法正确且不吞分号。
#define DS5DUMMY_DLOG(verbosity, ...) \
  do {                                 \
  } while (false)
#endif
