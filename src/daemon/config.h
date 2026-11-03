/**
 * @file daemon/config.h
 * @brief 启动配置：/etc 下的系统配置与目标用户家目录里的用户配置
 * @details 配置分两份。系统配置（/etc/ds5dummy/config.yaml）只由 root 写，
 *          决定 FunctionFS 路径、发现轮询间隔、系统日志级别与降权身份策略；
 *          用户配置
 *          （<home>/.config/ds5dummy/config.yaml）属于目标用户，只决定服务哪些
 *          手柄以及每只手柄的等待秒数、音频开关、音频/触觉增益、USB 轮询上限
 *          与个人日志级别。用户级设置按"内置默认 ← global ← controllers.<MAC>"
 *          三层叠加，解析完即每只手柄的最终值。这份文件是可选的：家目录里没有
 *          它，Manager 就按内置默认层启动（取值同发布的
 *          pkg/user-config.example.yaml），只有显式 `enabled: false` 才等于不启用。
 *          本模块做"YAML 文本 → 结构"和
 *          "加固的路径读取 → 文本"：不读环境变量、不查 passwd、不写日志；
 *          唯一的库查询是白名单 gid 的存在性（getgrgid_r），因为 gid 写错
 *          要到降权时才炸。失败一律通过返回值和 why 文本上报。
 * @details 用户配置是 root 进程读取的不可信输入：LoadUserConfig 用 O_NOFOLLOW
 *          逐级打开、校验普通文件、属主、权限与大小，解析后文本立即丢弃。
 * @note 层次：daemon。线程模型：Manager 启动阶段单线程调用一次，无全局状态。
 * @see daemon/options.h —— log_level 的级别名与整数语义复用其
 *      internal::ParseVerbosity。
 */

#pragma once

#include <sys/types.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ds5::daemon {

/// 系统配置的固定路径：只由 root 写，决定 ffs 路径、日志与身份策略。
inline constexpr const char* kSystemConfigPath = "/etc/ds5dummy/config.yaml";
/// 用户配置相对家目录的子目录（固定两级，LoadUserConfig 逐级 openat）。
inline constexpr const char* kUserConfigDir = ".config/ds5dummy";
/// 用户配置的文件名，与 kUserConfigDir 拼成 <home>/.config/ds5dummy/config.yaml。
inline constexpr const char* kUserConfigFileName = "config.yaml";

/**
 * @brief 降权身份的来源策略，对应系统配置里的 identity.mode。
 * @details 选择算法本身在 identity 模块（需要 logind 会话）：
 *          kAuto 要求恰好一个活跃图形会话；kList 只接受 allowed_users 名单内
 *          的会话，名单外用户等于未启用（服务正常退出而不是反复重启）。
 */
enum class IdentityMode {
  kAuto,  ///< 从活跃图形会话自动推导
  kList,  ///< 只接受 allowed_users 里显式列出的 uid/gid
};

/**
 * @brief 白名单模式下的一个允许用户。
 * @note gid 用配置里显式给出的主组（省略时等于 uid），因为降权前会清空补充
 *       组；与 passwd 主组不一致时由 Manager 打 WARNING（本模块只校验 gid
 *       存在于组库，不做比对）。
 */
struct IdentityEntry {
  uid_t uid = 0;  ///< 目标用户 uid，必须 > 0
  gid_t gid = 0;  ///< 降权后的主组 gid，必须 > 0；YAML 省略时取 uid
};

/**
 * @brief /etc/ds5dummy/config.yaml 的内容。
 * @details 缺省的键使用本结构的初始化值；未知键、类型不符与跨字段约束
 *          （mode: list 必须有 users）在解析期报错。
 * @note 纯数据，无行为；拷贝即配置快照。
 */
struct SystemConfig {
  /// FunctionFS 挂载点。进入 mkdir/mount 前还会过 usb::IsSafeGadgetPath。
  std::string ffs_path = "/dev/ffs-ds5";
  /// Manager 扫描 sysfs 找新手柄的间隔（秒），也是监督循环的节拍。调大减少
  /// sysfs 读取次数，代价是"手柄出现 / worker 退出"最多晚这么久才被处理。
  /// 必须 >= 1；上限按"秒 × 1000 能装进 int"收口，解析期校验。
  int discovery_interval_s = 3;
  /// loguru stderr 级别；缺省的 -1 就是 WARNING，是具体默认值而非哨兵。
  int log_level = -1;
  IdentityMode identity_mode = IdentityMode::kAuto;  ///< 身份策略，缺省 auto
  /// 白名单条目；只在 identity_mode == kList 时非空且有意义。
  std::vector<IdentityEntry> allowed_users;
};

/// 触觉增益的允许区间。配置解析与交接 argv 解析共用这一对边界，两处不能各写一份。
inline constexpr float kHapticsGainMin = 0.0f;
inline constexpr float kHapticsGainMax = 2.0f;
/// 扬声器前置增益的上限（3 位字段）；下限是 0。
inline constexpr int kSpeakerGainMax = 7;
/**
 * @brief usb_report_interval_ms 的合法取值（毫秒）：0 排在最前，它是模式而不是间隔。
 * @details 配置解析与交接 argv 解析共用这一张表，两处不能各写一份。这个值
 *          一路直用、全程不做除法：1/2/4 ms 就是全速中断端点 bInterval 的
 *          字节值，也是 IN 线程两份重发之间的间隔。0 是 real-time 档——只在
 *          收到新的蓝牙样本时写一份，描述符仍取最快的 1 ms。
 * @see daemon/config.cpp 的 ReadUsbReportInterval。
 */
inline constexpr int kUsbReportIntervals[] = {0, 1, 2, 4};

/**
 * @brief bt_addr 的三种取值形态，决定 Manager 服务哪些手柄。
 * @details 三种形态共用同一套发现机制（Manager 轮询 sysfs 里已建立 HID
 *          会话的 DualSense），差别只在"发现了要不要起 worker"：
 *          kAuto 全收、kList 只收 controllers 里列出的、kSingle 只收一个。
 */
enum class BtTargetMode {
  kAuto,    ///< 服务系统里发现的每一只 DualSense
  kList,    ///< 只服务 controllers 里列出的 MAC（该映射同时是白名单）
  kSingle,  ///< 只服务 bt_addr 里写的那一只
};

/**
 * @brief 一只手柄的生效设置，同时也是 global: 默认层的载体。
 * @details 字段与 worker 的交接参数一一对应（log_level 与 bt_addr 不在这里：
 *          前者只作用于 Manager 进程，后者决定服务哪只手柄）。
 * @note 纯数据，无行为；拷贝即配置快照。
 */
struct ControllerConfig {
  int bt_wait_s = 60;  ///< worker 重试 BT 连接的秒数；0 = 不等
  bool audio = true;   ///< false = 不建音频下链，只桥接 HID
  /// 触觉振幅倍率，[kHapticsGainMin, kHapticsGainMax]；1.0 = 原样，0 = 静音。
  /// 乘在 int8 量化之前，只影响下行触觉，不碰喇叭。
  float haptics_gain = 1.0f;
  /// 控制器侧扬声器前置增益（SetStateData byte[37] 低 3 位），[0, 7]。
  /// 固件初值是 2；字节随每份 0x36 送到手柄，由手柄自己放大喇叭。
  int speaker_gain = 2;
  /// 虚拟手柄两份 USB 输入报告之间的间隔（毫秒），见 kUsbReportIntervals：
  /// 1/2/4 即 1000/500/250 Hz，同一数值既是描述符里的 bInterval 字节，也是
  /// IN 线程自排的发送节拍。0 = real-time——描述符仍用最快的 1 ms，但 IN
  /// 线程只在收到新的蓝牙样本时写一份，不重发。蓝牙侧的上报节奏由手柄固件
  /// 固定，这里调的是主机取报告的节拍与重复份数。
  int usb_report_interval_ms = 1;
};

/**
 * @brief <home>/.config/ds5dummy/config.yaml 的内容。
 * @details 分层是"内置默认 ← global: ← controllers.<MAC>"：global 与每个
 *          controllers 条目都只写想改的键，解析期就把三层叠加成每只手柄的
 *          最终值，Manager 拿到的是可直接使用的快照。enabled 缺省 true，
 *          bt_addr 缺省 auto。
 * @note 纯数据，无行为；拷贝即配置快照。
 */
struct UserConfig {
  bool enabled = true;  ///< false = 该用户不启用本服务
  BtTargetMode bt_mode = BtTargetMode::kAuto;  ///< 服务哪些手柄；缺省全收
  /// kSingle 时的目标 MAC（规范大写）；其余模式为空串。
  std::string bt_addr;
  /// 覆盖系统级别的个人日志级别；std::nullopt = 继承系统值。用 optional 而不是
  /// 数值哨兵，因为 -1 本身是合法的 WARNING 级别。
  std::optional<int> log_level;
  /// global: 块叠加在内置默认上的结果；每只没有专属条目的手柄都用它。
  ControllerConfig defaults;
  /// MAC（规范大写）→ 叠加过 global 的最终设置。kList 模式要求非空，
  /// 其键集同时就是白名单。
  std::map<std::string, ControllerConfig> controllers;
};

/**
 * @brief 校验 MAC 文本是否为 XX:XX:XX:XX:XX:XX 形式。
 * @param addr 待校验字符串，大小写十六进制都接受。
 * @return 恰好 17 个字符、分隔符位置正确、其余为十六进制数字时 true。
 * @details 不做 OUI 或本机适配器归属校验——那需要 BlueZ 查询，格式合法已足以
 *          避免把任意字符串交给 BlueZ。配置解析与 sysfs 发现共用这一条判据。
 */
bool IsValidBtAddr(const std::string& addr);

/**
 * @brief 把 kUsbReportIntervals 写成面向用户的取值清单。
 * @return 形如 "0 (real-time), 1, 2 or 4" 的单行文本。
 * @details 配置解析与交接 argv 解析的"间隔取值非法"两处错误文本共用它，清单
 *          由表生成而不是手写，新增档位不会再多出一处要同步的文案。
 * @see ReadUsbReportInterval（config.cpp）、daemon/options.cpp 的
 *      --usb-report-interval-ms。
 */
std::string UsbReportIntervalChoices();

/**
 * @brief 把 MAC 文本规范成大写形式，作为 controllers 的键与手柄身份的比对口径。
 * @param addr MAC 文本，大小写不限；格式本身不在这里校验。
 * @return 逐字符转大写的结果（只动 a-f，其余原样）。
 * @details 配置里写的 MAC、sysfs 报告来的 MAC 都用它归一，两处因此不依赖
 *          手柄/用户各自的书写习惯。合法性由 IsValidBtAddr 单独负责。
 */
std::string CanonicalBtAddr(const std::string& addr);

/**
 * @brief 配置文件读取的结果分类。
 * @details 区分"文件本来就不存在"与"文件存在但不可用"：前者对两份配置都是
 *          正常状态（调用方按内置默认继续），后者必须报错并让 Manager 以非零码
 *          退出。
 */
enum class LoadResult {
  kOk,      ///< 读到并成功解析
  kAbsent,  ///< 路径不存在（ENOENT/ENOTDIR）；out 保持默认值，why 为空
  kError,   ///< 打开、校验、读取或解析失败；why 说明原因
};

/**
 * @brief 解析系统配置文本。
 * @param text YAML 文本；空文本按空文档处理，全部取默认值。
 * @param out 成功时写入解析结果；失败时不修改。
 * @param why 失败时写入面向用户的单行原因（含出错键名）；成功时清空。
 * @return 成功 true。
 * @details 严格模式：schema 之外的键、类型不符、identity.mode 非法、
 *          mode: list 缺 users、uid/gid <= 0、identity.users 条目既不是裸
 *          uid 也不是带 uid 的 mapping、mode: auto 却带了 users 都是错误。
 *          users 条目里的 gid 可以省略，省略时等于该条目的 uid。
 *          discovery_interval_s 必须是 >= 1 的整数，且秒数乘 1000 后仍能装进
 *          int——Manager 直接用这个乘积做毫秒节拍。
 *          解析先落在局部对象里，成功才覆盖 out，因此失败时 out 保持调用前的值。
 * @note 纯函数：不碰文件系统、不写日志，可在任意线程调用。
 * @see LoadSystemConfig —— 文件读取 + 本函数的组合。
 */
bool ParseSystemConfig(const std::string& text, SystemConfig& out,
                       std::string& why);

/**
 * @brief 解析用户配置文本。
 * @param text YAML 文本；空文本按空文档处理。
 * @param out 成功时写入解析结果；失败时不修改。
 * @param why 失败时写入面向用户的单行原因（含出错键名）；成功时清空。
 * @return 成功 true。
 * @details 严格模式：只接受 enabled/bt_addr/log_level/global/controllers 五个
 *          根键，ffs_path、uid、gid 等 root 级键在这里一律按未知键拒绝。
 *          global 与每个 controllers.<MAC> 条目都只接受 bt_wait_s/audio/
 *          haptics_gain/speaker_gain/usb_report_interval_ms 五个键。enabled 缺省 true，
 *          bt_addr 缺省 auto；bt_addr: list 要求 controllers 非空，
 *          bt_addr: <MAC> 要求 controllers 至多只有这一个键。
 *          两个增益按各自区间收口，usb_report_interval_ms 只接受 0/1/2/4
 *          （0 = real-time）。
 * @note 纯函数：不碰文件系统、不写日志，可在任意线程调用。
 * @see LoadUserConfig —— 加固读取 + 本函数的组合。
 */
bool ParseUserConfig(const std::string& text, UserConfig& out,
                     std::string& why);

/**
 * @brief 读取并解析系统配置文件。
 * @param path 配置文件路径（通常是 /etc/ds5dummy/config.yaml）。
 * @param out 成功时写入解析结果；kAbsent/kError 时不修改。
 * @param why 失败时写入原因；kAbsent 与成功时清空。
 * @return kOk 成功；kAbsent 文件不存在；kError 其它任何失败。
 * @details open 使用 O_NOFOLLOW，符号链接会被拒绝（ELOOP）；随后校验普通文件
 *          与 64 KiB 大小上限。系统文件不做属主校验——它由 root 创建，
 *          测试则以普通用户身份跑。解析通过后还要校验每个 allowed_users[i].gid
 *          都能在组库里解析（getgrgid_r）：gid 不存在是配置错误，报错文本里带
 *          条目下标与 gid。
 * @note 会分配内存并做磁盘 I/O，只在 Manager 启动阶段调用。
 */
LoadResult LoadSystemConfig(const std::string& path, SystemConfig& out,
                            std::string& why);

/**
 * @brief 读取并解析 <home>/.config/ds5dummy/config.yaml。
 * @param owner_uid 期望的文件属主（目标用户的 uid）；属主是 0（root）也接受。
 * @param home 目标用户的家目录绝对路径，来自 getpwuid_r。
 * @param out 成功时写入解析结果；kAbsent/kError 时不修改。
 * @param why 失败时写入原因；kAbsent 与成功时清空。
 * @return kOk 成功；kAbsent 任一路径分量不存在；kError 其它任何失败。
 * @details 从 home 开始逐级 openat：home → .config → ds5dummy → config.yaml，
 *          每一级都带 O_NOFOLLOW，且目录级打开的 fd 会再 fstat 一次，任何一级
 *          是符号链接都报错（含 home 本身）。文件必须是普通文件、属主为
 *          owner_uid 或 root、不可被组或其他用户写、且不超过 64 KiB。
 *          ENOENT 归一为 kAbsent 且不动 out——调用方因此拿到的是默认构造的
 *          结构，即内置默认层（与发布的用户模板取值一致，由单测钉住）；用户
 *          没有这份配置不等于不启用。其余错误（EACCES、符号
 *          链接等）都是 kError。
 * @note 会分配内存并做磁盘 I/O，只在 Manager 启动阶段调用。
 * @warning home 必须是可信来源（getpwuid_r 的结果），本函数不对它做路径白名单。
 */
LoadResult LoadUserConfig(uid_t owner_uid, const std::string& home,
                          UserConfig& out, std::string& why);

}  // namespace ds5::daemon
