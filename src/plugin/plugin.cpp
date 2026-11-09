/**
 * @file
 * @brief plugin 门面的实现：实例表、钩子表、装载策略与一个分派点
 *
 * 实例表与钩子表是本模块唯一的进程内状态：两张 vector，装载期写、运行期只读。
 * 它们不需要锁，靠的是与 bridge::mic_sink 同一条线程相位约定——@ref Start 在
 * 两条传输线程起来之前跑完，@ref Stop 在它们 join 之后才跑，@ref OnTick 因此在
 * 运行期并发只读。输入状态的钩子不在这里分派：@ref Start 把函数指针交给 worker，
 * worker 把它们注册进 bridge。
 *
 * 装载的顺序与判据都在这里，机制在 plugin_loader.cpp：目录里的每个 `.so` 先过
 * ELF 标记节，再 dlopen 拿导出名，然后过 abi_version、名字形态与 init/stop 必填
 * 这几关，最后拿到 init 返回 1。任何一关没过都只是这个插件缺席，不影响这一站，
 * 也不影响别的插件。装载不依赖配置：文件在插件目录里就会被装载，配置只负责把
 * 同名插件的键交过去。
 */

#include "plugin/plugin.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/debug.h"
#include "core/protocol/report.h"
#include "loguru.hpp"
#include "plugin/plugin_loader.h"

namespace plugin {
namespace {

/// 已装进来的插件描述符；插件是进程级单例，状态在它自己的静态存储里。
std::vector<const ds5PluginDesc*> instances;
/// 本轮装进来的 on_state_in 槽，按装载顺序；@ref Start 把这段区间交给 worker。
std::vector<StateInHook> state_in_hooks;

// 插件那份 C 边界里的状态体尺寸必须与协议常量同源，否则改写字节的插件会在
// 宿主内部被截断或越界。这条断言就是那条"只由一处定义"的连线。
static_assert(kPluginStateSize == core::protocol::kUsbInputStateSize,
              "ds5PluginDesc::on_state_in must hand over the whole input state");

/**
 * @brief 把一个描述符装进实例表：转配置、调 init。
 * @param desc 已通过形态检查的描述符。
 * @param settings 全部插件的扁平配置（可以是空的，插件用自己的默认值）。
 * @param host 交给插件的宿主能力表，abi_version 已由填它的那一方写好。
 * @return 进了实例表 true；init 返回 0（插件自己选择不生效）时 false。
 */
bool Add(const ds5PluginDesc* desc, const Settings& settings,
         const ds5Host& host) {
  const std::string name = desc->name;

  // 副本而不是 const_cast：init 拿到的必须是可改的左值，而表的内容我们不写，
  // 只在这一次调用里给它一份独立拷贝，插件误写也污染不到宿主的原表。
  ds5Host table = host;

  const std::vector<ds5Setting> items = detail::BuildSettings(settings, name);
  if (desc->init(&table, items.data()) == 0) {
    // 不是错误：配置把它关掉了，或它判定自己这一轮没活干（idle 的 0 走这里）。
    DS5DUMMY_LOG(INFO, "[plugin] {} not armed", name);
    return false;
  }
  instances.push_back(desc);
  if (desc->on_state_in != nullptr) state_in_hooks.push_back(desc->on_state_in);
  DS5DUMMY_LOG(INFO, "[plugin] loaded {} {}",
        name, desc->version != nullptr ? desc->version : "");
  return true;
}

/**
 * @brief 从可执行文件位置推出安装前缀：`<prefix>/bin/ds5dummyd` → `<prefix>`。
 * @return 前缀路径；`/proc/self/exe` 读不出来时返回空串。
 * @details 便携分发把整棵树装在任意前缀下（默认 `~/DS5Dummy`），插件在
 *          `<prefix>/plugins`，不在编译期固定的系统目录里。装到 `/usr/bin` 时
 *          推出来是 `/usr`，那一层的 `plugins/` 不存在，@ref detail::ListPlugins
 *          对不存在的目录返回空表，代价为零。
 */
std::string ExecutablePrefix() {
  std::error_code ec;
  const std::filesystem::path exe =
      std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) return {};
  return exe.parent_path().parent_path().string();
}

}  // namespace

std::vector<std::string> Directories() {
  std::vector<std::string> dirs{kSystemPluginDir};
  const std::string prefix = ExecutablePrefix();
  if (!prefix.empty()) dirs.push_back(prefix + "/plugins");
  const char* home = std::getenv("HOME");
  if (home != nullptr && *home != '\0') {
    dirs.push_back(std::string(home) + "/" + kUserPluginDir);
  }
  // Arch 包把整棵树装进 /usr/lib/ds5dummy 时，前缀层与系统目录是同一个路径。
  // 去掉重复项：同一个目录扫两遍会让每个插件多一次 dlopen，并多打一行
  // "同名覆盖"日志（两条日志里的路径还一模一样）。
  std::vector<std::string> unique;
  for (std::string& dir : dirs) {
    if (std::find(unique.begin(), unique.end(), dir) == unique.end()) {
      unique.push_back(std::move(dir));
    }
  }
  return unique;
}

std::span<const StateInHook> Start(const Settings& settings, const ds5Host& host,
                                   std::span<const std::string> dirs) {
  // 候选按插件名收口：同名只留一份。后一个目录里的那份覆盖前一个目录里的
  // （优先序由 Directories 给出，用户目录排在最后），同一个目录里出现两份时
  // 按文件名序留第一份。
  struct Found {
    std::string path;
    const ds5PluginDesc* desc;
    size_t dir;
  };
  std::map<std::string, Found> chosen;

  for (size_t dir_index = 0; dir_index < dirs.size(); ++dir_index) {
    for (const std::string& path : detail::ListPlugins(dirs[dir_index])) {
      std::string why;
      // 先看标记再 dlopen：目录里混进别的 .so 时连静态构造器都不跑。
      if (!detail::HasMarker(path, why)) {
        DS5DUMMY_LOG(INFO, "[plugin] {} skipped: {}", path, why);
        continue;
      }
      const ds5PluginDesc* desc = detail::Open(path, why);
      if (desc == nullptr) {
        DS5DUMMY_LOG(ERROR, "[plugin] {} not loaded: {}", path, why);
        continue;
      }
      // 版本号先比：不相等时描述符里后面那些指针一个都不能碰。
      if (desc->abi_version != kPluginAbiVersion) {
        DS5DUMMY_LOG(ERROR,
              "[plugin] {}: descriptor abi_version {} is not {}; not loaded",
              path, desc->abi_version, kPluginAbiVersion);
        continue;
      }
      if (desc->name == nullptr || !IsToken(desc->name)) {
        DS5DUMMY_LOG(ERROR, "[plugin] {}: descriptor name missing or not a token",
              path);
        continue;
      }
      if (desc->init == nullptr || desc->stop == nullptr) {
        DS5DUMMY_LOG(ERROR, "[plugin] {}: {} is missing init or stop", path,
              desc->name);
        continue;
      }

      const std::string name = desc->name;
      const auto found = chosen.find(name);
      if (found == chosen.end()) {
        chosen.emplace(name, Found{path, desc, dir_index});
      } else if (found->second.dir == dir_index) {
        DS5DUMMY_LOG(WARNING,
              "[plugin] {} also defines '{}'; keeping {}", path, name,
              found->second.path);
      } else {
        DS5DUMMY_LOG(INFO, "[plugin] {}: {} overrides {}", name, path,
              found->second.path);
        found->second = Found{path, desc, dir_index};
      }
    }
  }

  // map 按插件名有序：装载顺序与心跳顺序只取决于插件名字，不取决于目录内容
  // 的返回顺序。
  for (const auto& entry : chosen) {
    Add(entry.second.desc, settings, host);
  }
  return state_in_hooks;
}

void Stop() {
  // 先撤钩子再收实例：调用方按理已经在传输线程 join 之后清掉了 bridge 里的
  // 注册，这里清自己那份只是不让区间悬到下一轮 Start。
  state_in_hooks.clear();
  // 倒序收：装载时后装的插件可能依赖先装的那个还活着（目前没有这种插件，
  // 但顺序一旦定下来就只有一种解释）。不 dlclose：代码段留到进程结束。
  for (auto it = instances.rbegin(); it != instances.rend(); ++it) {
    (*it)->stop();
  }
  instances.clear();
}

void OnTick() {
  for (const ds5PluginDesc* desc : instances) {
    if (desc->on_tick != nullptr) desc->on_tick();
  }
}

}  // namespace plugin
