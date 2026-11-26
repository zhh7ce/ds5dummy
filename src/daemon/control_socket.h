/**
 * @file daemon/control_socket.h
 * @brief 本机控制 socket：任意本地用户发一条 `abort`，让 Manager 异常退出
 * @details Manager 在 `/run/ds5dummy/control.sock` 建一个 `AF_UNIX` 数据报监听点，
 *          模式 0666，不校验对端身份。一次 `sendto` 就是一个完整命令，没有连接、
 *          没有会话、没有回执；收到 `abort` 后不在这里做任何清理，只调用 shutdown
 *          的异常退出请求；真正的收摊由 Manager 主循环完成（见 shutdown.h）。
 *          systemd 的 `Restart=on-failure` 负责把这次非零退出变成一次重启，新进程
 *          会重新读取配置——这就是没有热加载时的"重启"入口。
 * @details 不负责：调用方身份校验、配置重载、worker 的生命周期、systemd 单元。
 * @note 层次：daemon。只在 root Manager 进程里用；单线程，监听 fd 由主循环在
 *       原有 100 ms 睡眠切片上 poll，不另起线程。
 * @see shutdown.h, daemon/manager.cpp
 */

#pragma once

#include <string>

namespace ds5::daemon::control_socket {

// 生产默认值就是 /run/ds5dummy；两个宏只给单测提供隔离路径，不改变运行期语义。
#ifndef DS5DUMMY_CONTROL_SOCKET_DIR
#define DS5DUMMY_CONTROL_SOCKET_DIR "/run/ds5dummy"
#endif
#ifndef DS5DUMMY_CONTROL_SOCKET_NAME
#define DS5DUMMY_CONTROL_SOCKET_NAME "control.sock"
#endif

/// @brief 控制 socket 所在目录；休眠钩子也在用它放 `was-active` 标记。
inline constexpr const char* kDirectory = DS5DUMMY_CONTROL_SOCKET_DIR;

/// @brief 控制 socket 的文件路径。
inline constexpr const char* kSocketPath =
    DS5DUMMY_CONTROL_SOCKET_DIR "/" DS5DUMMY_CONTROL_SOCKET_NAME;

/**
 * @brief 建目录、处理残留 socket、bind 并设为 0666。
 * @param why 失败原因；只在返回 false 时写入。
 * @return 成功 true；任何一步失败 false。
 * @details 目录已存在且确实是目录时继续（休眠钩子可能已建过它）；残留的 socket
 *          只有在确认无人监听时才 unlink。数据报 socket 不需要 `listen()`。
 * @note 失败时不会留下监听 fd；建出来的目录也不删——它可能被别处使用。
 */
bool Init(std::string& why);

/**
 * @brief 等待至多 timeout_ms 毫秒，并取走至多一个数据报。
 * @param timeout_ms 等待毫秒数；<=0 表示只做一次非阻塞检查。
 * @details 数据报一次 `recvfrom` 就是一条完整命令，没有"等一行"或读超时：
 *          `abort` 直接请求异常退出，其它内容记一条 WARNING 并忽略。没有数据的
 *          客户端根本不占服务端状态，所以不需要为它设任何等待上限。
 * @note 只在 Manager 单线程主循环上调用；信号打断 poll 时直接返回，由主循环去
 *       看退出标志。
 */
void PollOnce(int timeout_ms);

/**
 * @brief 关闭监听 fd 并删除自己创建的 socket 文件；可重复调用。
 * @note 只 unlink `kSocketPath`，不 rmdir `kDirectory`。
 */
void Close();

}  // namespace ds5::daemon::control_socket
