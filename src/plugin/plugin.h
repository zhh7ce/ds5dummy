/**
 * @file
 * @brief plugin 模块门面：装载插件，再把报告递到它们手上
 *
 * 插件是这个程序里唯一"内容由别人写"的一层。本模块负责三件事：把 .so 的描述符
 * 读进来、核对它的 ABI 版本号、把每个插件的心跳递过去；输入状态那条钩子不由
 * 本模块分派——它作为函数指针交给 worker，由 worker 注册进 bridge。
 * 本模块自己不做任何与手柄有关的决定——发什么报告、什么时候发，都在插件里。
 *
 * 插件看到的是 plugin/plugin_api.h 那份 C 边界，宿主内部看到的是本头。两侧之间
 * 没有 C++ 类型：配置的形态（@ref Settings）在 daemon 那一层就已经是字符串了。
 *
 * 装载不需要配置：@ref Start 扫 @ref Directories 给出的插件目录，把每个带
 * kPluginMarkerMagic 标记的 `.so` 装进来；文件放在目录里就等于启用。
 * 配置另有单独的一节：第一层是插件名，它以下想要几层就写几层，叶子必须是标量。
 * 这一节只负责把取值交给同名插件：
 *
 * @code
 * plugins:                      # 全局那一节
 *   idle:
 *     off_after_minutes: 30
 *   buttons:
 *     remap:                    # 组 → 源：比 idle 那一层多一层
 *       circle: cross
 * controllers:
 *   "AA:BB:CC:DD:EE:FF":
 *     plugins:                  # 逐手柄覆盖，只覆盖写了的那个叶子
 *       idle:
 *         off_after_minutes: 0
 * @endcode
 *
 * daemon 层在读取时就把嵌套摊平成 @ref Settings（键是 `插件名.段.段…`）：校验形状
 * （每段名字都是 token、叶子必须是非空标量、深度有上限）、逐层合并、经 worker 的
 * 配置快照下发。它不认识任何段名的含义，也不校验取值——那属于插件自己；"这一组有
 * 几层、每层叫什么"只由插件解释。配置里出现没有对应 `.so` 的插件名不是错误：
 * 那几条键没有接收方，装配照常。
 *
 * @note 进程级单实例，与 bridge/bt/usb 在同一个 worker 进程里；多手柄靠多个
 *       worker 进程，每站各自装载一份。插件本身也是进程级单例：没有 self，
 *       状态在插件自己的静态存储里。
 * @note 线程模型：@ref Start 与 @ref Stop 只在 worker 主线程上、两条传输线程起来
 *       之前 / join 之后调用（与 bridge::SetMicSink 同一个相位约定），所以实例表
 *       运行期只读、没有锁。@ref OnTick 在 worker 主循环上被调用；输入状态钩子由
 *       worker 在起线程前注册进 bridge，之后在 bt 读线程上被调用。
 * @warning 插件是进程内的任意代码：它能起线程、能改所有交给它的字节。本模块只
 *       保证"版本号不对的 .so 不会被打进进程"，不保证插件行为正确。
 * @see plugin/plugin_api.h —— 插件作者要 include 的那一份契约。
 * @see daemon/config.cpp —— 读 plugins: 那一节的地方。
 * @see daemon/options.cpp —— worker 配置快照里 plugins 的编解码。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "plugin/plugin_api.h"

namespace plugin {

/**
 * @brief 全部插件的配置：`插件名.点分键路径` → YAML 里写的标量文本。
 * @details 值是文本而不是类型化的量：这一层的读取方（daemon）不认识任何插件的
 *          键名，也就无法判断该按整数还是浮点解析；yaml-cpp 对任何标量都保留
 *          原文，摊平到字符串不丢信息。取值与校验发生在插件里，它拿到的是宿主
 *          转成的 ds5Setting 数组（见 ds5PluginDesc::init）。
 * @note 拍平之后就没有"切片"这个对象了：某个插件的配置是键带
 *       `插件名.` 前缀的那些条目。插件名段里不含 '.'（见 @ref IsToken），所以
 *       前缀匹配没有歧义；前缀之后的那些段原样交给插件，层数由配置文件决定、
 *       由插件自己解释（daemon 只把嵌套映射摊平成 @ref kKeySeparator 分隔的路径，
 *       见 daemon/config.cpp 的 FlattenPluginSettings）。
 * @note 用有序容器：同一份配置在每一次装载、每一份交接快照、每一行横幅里都
 *       给出同一个顺序，下游因此不需要各自再排一遍。同组的条目因为共享前缀，
 *       在这个顺序里自然相邻。
 */
using Settings = std::map<std::string, std::string>;

/// 扁平配置键里各段名字之间的分隔符（插件名与第一段、第一段与第二段……）。
inline constexpr char kKeySeparator = '.';

/// 系统插件目录：随包插件装在这里，与两个 PKGBUILD 的安装位置一致。
inline constexpr const char* kSystemPluginDir = "/usr/lib/ds5dummy/plugins";

/// 用户插件目录，相对家目录；同名插件在这里的一份覆盖系统目录里的那份。
inline constexpr const char* kUserPluginDir = ".config/ds5dummy/plugins";

/**
 * @brief 一段名字的合法形态：插件名、以及配置里插件名之下的每一段。
 * @param text 待校验字符串。
 * @return 非空、长度 <= 32、只含字母/数字/'_'/'-' 时 true。
 * @details 判据是"一段可辨识的标识符"：扁平键用 `.` 分隔插件名与下面各段名字，
 *          任何一段里带上分隔符就没法原样拆回来（也因此"写成一个带点的键"不是
 *          "写成嵌套"的另一种拼法——前者在这里被拒）。长度上限挡住"把整段说明写
 *          进键名"；配置解析与目录扫描共用这一条判据，两侧不会各自演化。描述符
 *          里插件名的校验也走这里。
 * @see daemon/config.cpp —— 读 plugins: 那一节时用它校验形状。
 */
inline bool IsToken(const std::string& text) {
  if (text.empty() || text.size() > 32) return false;
  for (const char c : text) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

/**
 * @brief 一份输入状态钩子：与 bridge::StateHook 同一个类型。
 *
 * 插件是进程级单例，所以这里就是个裸函数指针；worker 拿它注册进 bridge，
 * 两条传输线程起来之后由 bridge 在 bt 读线程上调。
 */
using StateInHook = void (*)(uint8_t* state);

/**
 * @brief 扫插件目录里的 `.so`，逐个 init。
 *
 * @param dirs 要扫描的目录，按优先级从低到高；后一个目录里的同名插件覆盖前一个
 *             目录里的那一个（用户目录因此覆盖系统目录）。目录不存在、为空都
 *             不影响启动。只有以 `.so` 结尾的普通文件是候选，先看 ELF 标记节
 *             （@ref detail::HasMarker）再 dlopen——目录里放错的文件不会被跑起来。
 *
 * 每个插件拿到的配置是 @p settings 里键带 `插件名.` 前缀的那些条目（没有就是
 * 空数组，插件用自己的默认值）；配置里的插件名与装载到什么无关。init 返回 0 的
 * 插件（被配置关掉、或它自己判定条件不成立）不进实例表，之后不会被调用任何钩子。
 * 装载失败的插件只让它自己缺席：记 ERROR 并继续，一颗 .so 读不进来不是这一站的
 * 问题。装载顺序按插件名排序，与目录返回顺序无关。
 *
 * @param settings 本站生效的插件配置，来自用户配置并按手柄合并过。
 * @param host 宿主能力表，它的 abi_version 由填它的那一方写好，本模块原样交给
 *             每个插件的 init。本函数返回后不再需要 @p host 活着——递给插件的
 *             是它的一份副本。
 * @return 本轮装进来的 on_state_in 钩子，按装载顺序；区间指向模块内的钩子表，
 *         在下一次 @ref Stop 之前有效。没有钩子时是空区间（常态：没装插件、
 *         插件自己选择了不生效）。
 * @warning 必须在 bt::StartThreads() 之前调用。Start 不再自己收上一批：除首次
 *          外，调用方必须先 @ref Stop（worker 的 Stop 路径负责这一步）。
 * @see Stop() —— 反向的那一步。
 */
std::span<const StateInHook> Start(const Settings& settings, const ds5Host& host,
                                   std::span<const std::string> dirs);

/**
 * @brief 默认插件目录，按优先级从低到高。
 * @return 依次是 kSystemPluginDir、可执行文件所在前缀的 `plugins/`（推不出前缀
 *         时缺席）、$HOME 可用时的 $HOME/.config/ds5dummy/plugins。
 * @details 中间那一层是便携分发用的：整棵树装在 `~/DS5Dummy` 这类前缀下时，
 *          随包插件在 `<prefix>/plugins`，环境变量里没有它的位置，只能按
 *          `/proc/self/exe` 现推；装到 `/usr` 时推出来的 `/usr/plugins` 不存在，
 *          扫描它对不存在的目录返回空表。
 * @details HOME 由 worker 在降权之后修成目标用户的家目录（见
 *          daemon/worker.cpp 的 RestoreHome），所以这里拿到的就是用户的插件目录。
 *          同名插件以后出现的目录为准：用户目录覆盖随包的那两份。
 * @note 重复的路径只留第一次出现的那一条：Arch 包把整棵树装在
 *       `/usr/lib/ds5dummy` 时，前缀层与 kSystemPluginDir 就是同一个目录。
 */
std::vector<std::string> Directories();

/**
 * @brief 收掉全部插件：对每个实例调用它的 stop 槽，然后清空实例表。
 *
 * 只做这一件事：不 dlclose。已经映射进来的代码段留到进程结束——拆一个可能
 * 自己起过线程的 .so，风险比省下的那点地址空间大。重复调用无害。
 *
 * @warning 只能在两条传输线程 join 之后调用：插件的 stop 要 join 它自己的线程，
 *          而它的钩子可能正在 bt/usb 的线程上跑。
 * @see Start() —— 反向的那一步。
 */
void Stop();

/**
 * @brief 心跳：把 on_tick 递给每个插件。
 *
 * 无参数：要计时的插件自己取时钟，这样"静置了多久"不取决于主循环这一轮睡了
 * 多久。
 *
 * @warning 只在 worker 主循环（100 ms 一轮）上调用；这是插件唯一被允许动链路
 *          的时刻，host 的 power_off 也只有从这里开始的调用是合法的。
 * @see ds5PluginDesc::on_tick。
 */
void OnTick();

}  // namespace plugin
