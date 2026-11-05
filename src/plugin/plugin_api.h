/**
 * @file
 * @brief 插件的 ABI 边界：一份描述符、一份宿主接口，两边只用 C 结构体对话
 *
 * 插件以独立 .so 交付：导出名为 kPluginEntryName 的变量，文件里再把
 * kPluginMarkerMagic 这个字符串放进 `.ds5_plugin` 节当识别标记；宿主扫插件目录
 * 时先认这个标记，再 dlopen。插件作者与宿主各自编译同一份声明，链接期没有任何
 * C++ 类型跨过去。
 *
 * 结构体里没有虚表、没有 std:: 类型、没有异常穿过任何一次调用，全部成员是整型、
 * 指针和函数指针。因此 std::is_standard_layout 成立，两边的偏移一致；插件作者用
 * C++ 写本头以上的代码，纯 C 作者照抄这些布局也能得到同一个 ABI（本头本身只能被
 * C++ 包含，末尾两条 static_assert 用的就是 std::is_standard_layout）。
 *
 * 一份描述符是不是宿主认得的形态，只由两样东西决定：**导出名**（.so 里必须是
 * kPluginEntryName）与**版本号**（abi_version 必须等于 kPluginAbiVersion）。
 * 版本号是任何布局改动的唯一开关：删字段、改顺序、改函数签名、在尾部追加字段，
 * 都得递增它——结构体里没有 size，宿主分不出一个较短的旧结构，只能整份拒绝。
 *
 * @note 函数签名不匹配是**静默**的：ELF 的 .dynsym 只带名字、地址、代码字节数与
 *       类型，不带参数类型。x86-64 SysV 下多出来的实参被丢弃、缺的实参读到寄存器
 *       里的残留，整型类与 SSE 类放错位置也一样能返回——不会立刻崩。所以签名改动
 *       必须走版本号，不能指望运行期帮你发现。
 * @note 宿主不提供时钟：要计时的插件自己取 std::chrono::steady_clock（Linux 上是
 *       CLOCK_MONOTONIC，不含挂起时间），或者自己攒状态。常用的取法、配置键查找
 *       与整数解析在 plugin/plugin_util.h 里有一份现成的 inline 实现。
 * @see plugin/plugin_util.h —— 可选的头文件工具，纯 inline，不进 ABI。
 * @see plugin/plugin.h —— 宿主侧的装载与分派门面。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

/// .so 里必须导出的描述符变量名：dlsym 只认这一个字符串。
inline constexpr char kPluginEntryName[] = "ds5_plugin_desc";

/// 本头这份布局的版本号；装载时先比它，不相等就一个字节都不碰。
inline constexpr uint32_t kPluginAbiVersion = 1;

/// 一次输入状态体的字节数；与 core::protocol::kUsbInputStateSize 同值，
/// 那条同值关系由宿主侧（plugin/plugin.cpp）的 static_assert 钉住。
inline constexpr std::size_t kPluginStateSize = 63;

/// 承载识别标记的 ELF 节名；装载器不 dlopen 任何没有这个节的 .so。
/// @note `section` 属性只接受字符串字面量，插件里定义标记时要照抄这个字符串，
///       不能用这个常量（它只用来读节名）。
inline constexpr char kPluginMarkerSection[] = ".ds5_plugin";

/**
 * @brief 识别标记的内容：插件把它逐字节放进 `.ds5_plugin` 节（含结尾的 NUL）。
 *
 * 插件在文件作用域里定义一份：
 * @code
 * __attribute__((used, section(".ds5_plugin")))
 * const char ds5_plugin_marker[] = "DS5 Plugin";
 * @endcode
 *
 * @details 插件目录由宿主扫描，目录里任何 `.so` 都会先被打开看节表；没有这个节
 *          或内容不是这个字符串的文件直接跳过，不会 dlopen、不会跑它的静态构造
 *          器。这是"是不是我们的插件"的第一道判据，第二道是导出名与 abi_version。
 * @note 标记是必需项：漏了它的 .so 即使导出名与版本号都对，也永远不会被装载。
 * @warning 只有 GCC/Clang 的 `section` 属性可用；Linux 之外没有这条路。
 */
inline constexpr char kPluginMarkerMagic[] = "DS5 Plugin";

/// 宿主日志的四档，语义与 daemon 的 -v 同一族词汇。
enum ds5LogLevel {
  DS5_LOG_ERROR = 0,    ///< 插件没能做成它想做的事。
  DS5_LOG_WARNING = 1,  ///< 做成了，但有用户该知道的偏差（比如配置取值回落）。
  DS5_LOG_INFO = 2,     ///< 里程碑：装好了、开始生效了、到期动作发了。
  DS5_LOG_DEBUG = 3     ///< 高频细节；只在诊断构建里看得到的话就少说。
};

/**
 * @brief 宿主交给插件的能力：一行日志，和一条让手柄关机的通道。
 *
 * 插件能做的一切都在这个结构体里，没有第二条路：它不 include bridge/bt，也不
 * 靠符号引用去够宿主的东西（主程序不导出符号，真去引用会在 dlopen 时当场失败）。
 */
struct ds5Host {
  /// 本结构体的版本；与插件编译时的 kPluginAbiVersion 不等时一个成员都不要读，
  /// init 直接返回 0。
  uint32_t abi_version;

  /// 写一行宿主日志。@p message 是已经排好版的单行文本，宿主不改它、不解释其中的
  /// 百分号。任何线程可调用。
  void (*log)(int level, const char* message);

  /**
   * @brief 让手柄自己关机（feature 报告 0x08）。
   * @return 已交给链路 1；失败 0，此时手柄继续开着。
   * @note 等价于一次内容冻结的 feature SET_REPORT，单列出来是因为"让手柄断电"
   *       是插件最常要做的决定，而那份报告的字节（含尾部 CRC）属于宿主不属于
   *       插件。
   * @warning 只能从 on_tick 里调，且要当它是**同步**的：这次写会和 ep0 线程上
   *          现取的 GET_REPORT 排同一把锁，最长可能等到手柄回一份 feature
   *          （预算 2 s）。在 on_state_in 那类高频钩子上调它就是把输入链停住。
   */
  int (*power_off)(void);
};

/**
 * @brief 一条插件配置：键与值各一个字符串。
 *
 * 宿主手里本来就是分开的两份文本（摊平后的键路径与 YAML 标量原文），所以按对
 * 交出来，插件不必再按 '=' 把一条字符串切开两次。数组以 {nullptr, nullptr}
 * 结尾。
 */
struct ds5Setting {
  /// 本插件配置里的键路径，不含 `插件名.` 前缀：配置文件里那一节怎么分层，
  /// 这里就怎么写，段与段之间是 '.'（如 off_after_minutes、remap.circle）。
  /// 段数不作约定：一层还是三层只由接收它的插件解释。
  const char* key;
  const char* value;  ///< YAML 里写的标量原文。
};

/**
 * @brief 插件对外交出的全部东西：三样身份 + 四个槽。
 *
 * 插件是**进程级单例**：状态放在实现自己的静态存储里，描述符不携带实例指针，
 * 每个钩子都不收 self。槽位为空（nullptr）就是"这个插件不做这件事"，宿主跳过
 * 空槽；init/stop 必填。
 */
struct ds5PluginDesc {
  /// 必须等于 kPluginAbiVersion，否则宿主拒绝装载。
  uint32_t abi_version;
  /// 插件名：配置里 `plugins:` 那一节的键名，也是日志里的自称。必须是宿主
  /// plugin::IsToken 认得的形态（字母/数字/_/-，<= 32 字节），否则装载被拒。
  const char* name;
  /// 插件自己的版本，自由文本，只进日志；宿主不解释、不比较。
  const char* version;

  /**
   * @brief 初始化：读配置、把静态状态武装起来。
   * @param host     宿主能力表，只在本次调用期间有效——要长期用得自己存一份。
   *                 插件必须先核对它的 abi_version，再读其余成员。
   * @param settings 本插件的配置项，以 {nullptr, nullptr} 结尾；没有配置项时
   *                 是只含尾项的一元素数组（不是空指针本身，不过实现可以像
   *                 随包的 idle 那样两者都接）。键是配置文件里的键路径（段间用
   *                 '.'，见 ds5Setting::key），宿主不解释任何段名。键不认识就
   *                 忽略，取值不合法由插件自己回落默认值并记 WARNING。字符串只
   *                 在本次调用期间有效。
   * @return 1 = 这一轮生效，宿主此后会调它的钩子；0 = 不生效（配置关掉了它、或
   *         它需要的东西不在），宿主记 INFO，不再调它的任何槽。
   * @warning 在 worker 主线程上、两条传输线程起来之前调用；可以分配内存、可以
   *          起自己的线程（起了就要在 stop 里收干净）。不许阻塞在链路操作上。
   */
  int (*init)(struct ds5Host* host, const struct ds5Setting* settings);

  /**
   * @brief 收摊：join 自己起的线程、释放 init 分配的一切，并把静态状态复位。
   * @note 只在 init 返回过 1 时被调用一次；之后宿主不再碰这个插件。
   */
  void (*stop)(void);

  /**
   * @brief 心跳：worker 主循环每 100 ms 调一次，无参数。
   * @details 这是唯一一条宿主承诺"可以动链路"的钩子：要检查自己的计时、发报告，
   *          都在这里。它不在任何传输线程上，所以 host 的执行器可以同步阻塞而
   *          不拖慢输入流。需要时间就自己取时钟——宿主不递步长，这样计时精度
   *          不取决于主循环这一轮睡了多久。没活干就立刻返回。
   */
  void (*on_tick)(void);

  /**
   * @brief 手柄上报了一份输入状态：改写字节或直接读。
   * @param state kPluginStateSize（63）字节状态体，可写；宿主在你返回之后才把它
   *              摆成发给主机的 USB 输入报告，所以改动立即生效。
   * @warning 在 bt 的读线程上、约 100–120 Hz 调用：不许阻塞、不许分配、不许调用
   *          host 的执行器（会把控制通道上的锁和发送队列的背压搬到这条线程上）。
   *          要动作就把结论记进自己的状态，等 on_tick。
   */
  void (*on_state_in)(uint8_t state[kPluginStateSize]);
};

static_assert(std::is_standard_layout_v<ds5Setting>,
              "ds5Setting must stay a plain C struct");
static_assert(std::is_standard_layout_v<ds5Host>,
              "ds5Host must stay a plain C struct");
static_assert(std::is_standard_layout_v<ds5PluginDesc>,
              "ds5PluginDesc must stay a plain C struct");
