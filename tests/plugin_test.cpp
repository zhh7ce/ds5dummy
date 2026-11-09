/**
 * @file
 * @brief plugin 模块的单测：目录扫描、标记识别、装载与分派（无需 root/手柄）
 *
 * 覆盖的是"插件目录里一个 .so 能不能变成一个会跑回调的插件"这条链路：
 * - 目录扫描：只认 `.so` 普通文件，不存在/为空的目录不影响启动；目录顺序是
 *   系统 → 可执行文件前缀（便携分发） → 用户，后者覆盖前者；
 * - 标记识别：没有 `.ds5_plugin` 节的 .so 在 dlopen 之前就被跳过；
 * - dlopen 路径：真加载测试期间构建出来的 .so，并证明它的钩子真的被调用过；
 * - 同名去重与目录优先级、版本号不符的 .so 在装载点被拒；
 * - 随包 idle 与 buttons 的真实产物从目录里被装载，buttons 那几条一路走到宿主
 *   最终发出去的那些字节；
 * - Start 交出的输入钩子区间与空槽的跳过。
 *
 * 每个用例自己搭一个临时插件目录，从不扫真实的系统/用户目录，所以不需要 root，
 * 也不会被这台机器上装没装插件影响。日志与 power_off 都由测试自制的 ds5Host
 * 提供，不碰 bt/usb。
 */

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "plugin/plugin.h"
#include "plugin/plugin_loader.h"

namespace {

using plugin::Settings;

namespace fs = std::filesystem;

/// 测试夹具 .so 的绝对路径，由 CMake 在构建期告知。
constexpr const char* kProbePath =
#ifdef DS5DUMMY_TEST_PROBE_SO
    DS5DUMMY_TEST_PROBE_SO;
#else
    "";
#endif

/// 版本号被改成宿主不认的那份夹具。
constexpr const char* kProbeBadAbiPath =
#ifdef DS5DUMMY_TEST_PROBE_BAD_ABI_SO
    DS5DUMMY_TEST_PROBE_BAD_ABI_SO;
#else
    "";
#endif

/// 导出名与版本号都对、但没有标记节的夹具。
constexpr const char* kProbeNoMarkerPath =
#ifdef DS5DUMMY_TEST_PROBE_NO_MARKER_SO
    DS5DUMMY_TEST_PROBE_NO_MARKER_SO;
#else
    "";
#endif

/// 随包 idle 插件的真实产物，由 CMake 在构建期告知。
constexpr const char* kIdlePath =
#ifdef DS5DUMMY_TEST_IDLE_SO
    DS5DUMMY_TEST_IDLE_SO;
#else
    "";
#endif

/// 随包 buttons 插件的真实产物。
constexpr const char* kButtonsPath =
#ifdef DS5DUMMY_TEST_BUTTONS_SO
    DS5DUMMY_TEST_BUTTONS_SO;
#else
    "";
#endif

int g_power_off_calls = 0;

void Log(int /*level*/, const char* /*message*/) {}

int PowerOff() {
  ++g_power_off_calls;
  return 1;
}

/// 一份宿主表：装载与分派只需要版本号、日志与关机通道。
ds5Host MakeHost() {
  ds5Host host{};
  host.abi_version = kPluginAbiVersion;
  host.log = &Log;
  host.power_off = &PowerOff;
  return host;
}

class PluginTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_power_off_calls = 0;
    plugin::Stop();  // 上一条用例可能留了实例：每轮从空表开始

    std::string pattern =
        (fs::temp_directory_path() / "ds5dummy-plugins-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const char* created = ::mkdtemp(buffer.data());
    ASSERT_NE(created, nullptr);
    root_ = created;
  }

  void TearDown() override {
    plugin::Stop();
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  /// 在临时根下建一个子目录并返回路径。
  std::string Dir(const std::string& name) const {
    const fs::path dir = fs::path(root_) / name;
    std::error_code ec;
    fs::create_directories(dir, ec);
    EXPECT_FALSE(ec) << ec.message();
    return dir.string();
  }

  /// 把一个构建产物复制进插件目录，文件名保持不变。
  void Install(const std::string& dir, const char* source) const {
    std::error_code ec;
    fs::copy_file(source, fs::path(dir) / fs::path(source).filename(),
                  fs::copy_options::overwrite_existing, ec);
    EXPECT_FALSE(ec) << ec.message();
  }

  /// 单目录的 Start 调用。
  std::span<const plugin::StateInHook> Start(const Settings& settings,
                                             const ds5Host& host,
                                             const std::string& dir) {
    dirs_.assign(1, dir);
    return plugin::Start(settings, host, dirs_);
  }

  std::string root_;
  std::vector<std::string> dirs_;
};

// ---------------------------------------------------------------------------
// 目录扫描与标记识别
// ---------------------------------------------------------------------------

TEST_F(PluginTest, ListPluginsOnlyReturnsSoRegularFiles) {
  const std::string dir = Dir("scan");
  {
    std::ofstream(dir + "/notes.txt") << "not a plugin";
    std::ofstream(dir + "/broken.so") << "not an ELF file";
  }
  std::error_code ec;
  fs::create_directories(dir + "/sub.so", ec);  // 名字像插件，但它是目录

  const std::vector<std::string> found = plugin::detail::ListPlugins(dir);
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(fs::path(found[0]).filename().string(), "broken.so");
}

TEST_F(PluginTest, ListPluginsOnMissingDirectoryIsEmpty) {
  EXPECT_TRUE(plugin::detail::ListPlugins(root_ + "/nope").empty());
}

/// 临时换掉 HOME，析构时恢复：Directories() 读的就是这个环境变量。
struct HomeOverride {
  std::string saved;
  bool had = false;

  explicit HomeOverride(const char* value) {
    const char* old = std::getenv("HOME");
    had = old != nullptr;
    if (had) saved = old;
    ::setenv("HOME", value, 1);
  }
  ~HomeOverride() {
    if (had) {
      ::setenv("HOME", saved.c_str(), 1);
    } else {
      ::unsetenv("HOME");
    }
  }
};

// 顺序就是装载优先级：用户目录必须排在最后才可能覆盖随包插件；中间那一层是
// 便携分发的前缀（按 /proc/self/exe 现推，测试二进制的上一个目录就是构建目录）。
TEST_F(PluginTest, DirectoriesPutTheUserDirLastAndDeriveThePrefix) {
  const HomeOverride home("/tmp/ds5dummy-test-home");
  const std::vector<std::string> dirs = plugin::Directories();

  ASSERT_EQ(dirs.size(), 3u);
  EXPECT_EQ(dirs[0], plugin::kSystemPluginDir);

  std::error_code ec;
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  ASSERT_FALSE(ec) << ec.message();
  EXPECT_EQ(dirs[1], (exe.parent_path().parent_path() / "plugins").string());

  EXPECT_EQ(dirs[2],
            std::string("/tmp/ds5dummy-test-home/") + plugin::kUserPluginDir);
}

TEST_F(PluginTest, HasMarkerRejectsAFileThatIsNotElf) {
  const std::string path = root_ + "/plain.so";
  std::ofstream(path) << "not an ELF file";
  std::string why;
  EXPECT_FALSE(plugin::detail::HasMarker(path, why));
  EXPECT_FALSE(why.empty());
}

#ifdef DS5DUMMY_TEST_PROBE_SO

TEST_F(PluginTest, HasMarkerAcceptsAPluginBuiltFromTheAbiHeader) {
  std::string why;
  EXPECT_TRUE(plugin::detail::HasMarker(kProbePath, why)) << why;
}

#endif  // DS5DUMMY_TEST_PROBE_SO

#ifdef DS5DUMMY_TEST_PROBE_NO_MARKER_SO

TEST_F(PluginTest, HasMarkerRejectsAPluginWithoutTheMarkerSection) {
  std::string why;
  EXPECT_FALSE(plugin::detail::HasMarker(kProbeNoMarkerPath, why));
  EXPECT_NE(why.find(".ds5_plugin"), std::string::npos);
}

#endif  // DS5DUMMY_TEST_PROBE_NO_MARKER_SO

// ---------------------------------------------------------------------------
// loader：dlopen 与配置数组
// ---------------------------------------------------------------------------

TEST_F(PluginTest, OpenRejectsEmptyPathWithoutDlopen) {
  // dlopen("") 不失败，它给的是主程序的句柄——那正是我们不导出插件符号的地方。
  std::string why;
  EXPECT_EQ(plugin::detail::Open("", why), nullptr);
  EXPECT_FALSE(why.empty());
}

TEST_F(PluginTest, OpenReportsMissingFile) {
  std::string why;
  EXPECT_EQ(plugin::detail::Open("/nonexistent/ds5dummy_plugin.so", why),
            nullptr);
  EXPECT_FALSE(why.empty());
}

TEST_F(PluginTest, BuildSettingsProducesKeyValueArray) {
  Settings settings;
  settings["idle.off_after_minutes"] = "5";
  settings["idle.mode"] = "keep-alive";
  settings["other.key"] = "ignored";  // 别的插件的键不该混进来
  const std::vector<ds5Setting> items =
      plugin::detail::BuildSettings(settings, "idle");

  ASSERT_EQ(items.size(), 3u);  // 两条配置 + 结尾的 {nullptr, nullptr}
  EXPECT_EQ(items[2].key, nullptr);
  EXPECT_EQ(items[2].value, nullptr);
  // 前缀被剥掉、键与值分开给：插件不用再按 '=' 切字符串；顺序按键名排序。
  EXPECT_STREQ(items[0].key, "mode");
  EXPECT_STREQ(items[0].value, "keep-alive");
  EXPECT_STREQ(items[1].key, "off_after_minutes");
  EXPECT_STREQ(items[1].value, "5");
}

TEST_F(PluginTest, BuildSettingsOnEmptySettingsIsJustTheTerminator) {
  const std::vector<ds5Setting> items =
      plugin::detail::BuildSettings(Settings{}, "idle");
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].key, nullptr);
  EXPECT_EQ(items[0].value, nullptr);
}

// ---------------------------------------------------------------------------
// Start / 分派 / Stop
// ---------------------------------------------------------------------------

TEST_F(PluginTest, EmptyDirectoryLoadsNothing) {
  EXPECT_TRUE(Start(Settings{}, MakeHost(), Dir("empty")).empty());
}

TEST_F(PluginTest, TickDispatchIsHarmlessWithNoPlugins) {
  plugin::OnTick();
  SUCCEED();
}

#ifdef DS5DUMMY_TEST_PROBE_SO

TEST_F(PluginTest, ProbeInThePluginDirectoryIsLoadedAndDispatched) {
  const std::string dir = Dir("probe");
  Install(dir, kProbePath);

  // 配置里没有这个插件的任何键：装载与配置无关。
  const std::span<const plugin::StateInHook> hooks =
      Start(Settings{}, MakeHost(), dir);
  ASSERT_EQ(hooks.size(), 1u);

  uint8_t state[kPluginStateSize] = {};
  hooks[0](state);
  EXPECT_EQ(state[0], 0x5A);  // 真的跑到了 .so 里的代码
}

TEST_F(PluginTest, TwoFilesWithTheSamePluginNameLoadOnce) {
  const std::string dir = Dir("duplicates");
  Install(dir, kProbePath);
  std::error_code ec;
  fs::copy_file(kProbePath, dir + "/another.so",
                fs::copy_options::overwrite_existing, ec);
  ASSERT_FALSE(ec) << ec.message();

  // 两个文件都自报 "probe"：进程级单例，只能装一份，否则 init/stop、心跳与
  // 输入钩子都会成对出现。
  EXPECT_EQ(Start(Settings{}, MakeHost(), dir).size(), 1u);
}

TEST_F(PluginTest, LaterDirectoryOverridesTheSamePluginName) {
  const std::string system_dir = Dir("system");
  const std::string user_dir = Dir("user");
  Install(system_dir, kProbePath);
  Install(user_dir, kProbePath);

  std::vector<std::string> dirs{system_dir, user_dir};
  EXPECT_EQ(plugin::Start(Settings{}, MakeHost(), dirs).size(), 1u);
}

TEST_F(PluginTest, StopThenStartRebuildsHooks) {
  const std::string dir = Dir("restart");
  Install(dir, kProbePath);
  const ds5Host host = MakeHost();
  ASSERT_EQ(Start(Settings{}, host, dir).size(), 1u);
  // 区间活到下一次 Stop；Stop 之后重新装载，拿到的是一份新的。
  plugin::Stop();
  ASSERT_EQ(Start(Settings{}, host, dir).size(), 1u);
}

#endif  // DS5DUMMY_TEST_PROBE_SO

#ifdef DS5DUMMY_TEST_PROBE_NO_MARKER_SO

TEST_F(PluginTest, SharedObjectWithoutTheMarkerIsSkippedBeforeDlopen) {
  const std::string dir = Dir("markerless");
  Install(dir, kProbeNoMarkerPath);
  // 导出名、版本号、名字形态都对，唯独没有标记节：装载点看文件字节就跳过它，
  // 不会 dlopen，也不会调用它的 init。
  EXPECT_TRUE(Start(Settings{}, MakeHost(), dir).empty());
}

#endif  // DS5DUMMY_TEST_PROBE_NO_MARKER_SO

#ifdef DS5DUMMY_TEST_PROBE_BAD_ABI_SO

TEST_F(PluginTest, StartRejectsDescriptorWithForeignAbiVersion) {
  const std::string dir = Dir("bad-abi");
  Install(dir, kProbeBadAbiPath);
  // 标记与导出名都对，只有版本号不是宿主这份：装载点当场拒掉，既不 init 也
  // 不把它的 on_state_in 交出来。
  EXPECT_TRUE(Start(Settings{}, MakeHost(), dir).empty());
}

#endif  // DS5DUMMY_TEST_PROBE_BAD_ABI_SO

#ifdef DS5DUMMY_TEST_IDLE_SO

TEST_F(PluginTest, ShippedIdleLoadsFromThePluginDirectory) {
  const std::string dir = Dir("shipped");
  Install(dir, kIdlePath);
  // 随包的 idle.so 走与第三方插件完全相同的扫描 + 标记 + dlopen 路径；它默认
  // 武装，并且填了 on_state_in，所以交出一个钩子。
  EXPECT_EQ(Start(Settings{}, MakeHost(), dir).size(), 1u);
}

TEST_F(PluginTest, ShippedIdleOptsOutWhenItsConfigDisablesIt) {
  const std::string dir = Dir("idle-off");
  Install(dir, kIdlePath);
  Settings settings;
  settings["idle.off_after_minutes"] = "0";
  EXPECT_TRUE(Start(settings, MakeHost(), dir).empty());
}

#endif  // DS5DUMMY_TEST_IDLE_SO

#ifdef DS5DUMMY_TEST_BUTTONS_SO

TEST_F(PluginTest, ShippedButtonsRemapsThroughTheRealLoadingPath) {
  const std::string dir = Dir("buttons");
  Install(dir, kButtonsPath);
  Settings settings;
  settings["buttons.remap.circle"] = "cross";

  // 三层键（插件名.组.源）摊平后原样到达 init，钩子交回的是真的改写能力：
  // ○ 进去、✕ 出来，中间没有任何测试专用的接线。
  const auto hooks = Start(settings, MakeHost(), dir);
  ASSERT_EQ(hooks.size(), 1u);
  std::uint8_t state[kPluginStateSize] = {};
  state[7] = 0x08 | 0x40;  // 十字键未按 + ○。
  hooks[0](state);
  EXPECT_EQ(state[7], 0x08 | 0x20);
}

TEST_F(PluginTest, ShippedButtonsStaysUnarmedWithoutItsSection) {
  const std::string dir = Dir("buttons-off");
  Install(dir, kButtonsPath);
  // C-04：`buttons` 整节没写 = 整个插件不生效，一个钩子都不交出来。
  EXPECT_TRUE(Start(Settings{}, MakeHost(), dir).empty());
}

TEST_F(PluginTest, ShippedButtonsIgnoresKeysItDoesNotRead) {
  const std::string dir = Dir("buttons-unknown");
  Install(dir, kButtonsPath);
  Settings settings;
  settings["buttons.gesture.swipe_up"] = "circle";
  settings["buttons.remap.cricle"] = "cross";
  // 还没实现的功能组与写错的键名都只到"忽略"为止：不武装，也不让服务退出（G-06）。
  EXPECT_TRUE(Start(settings, MakeHost(), dir).empty());
}

TEST_F(PluginTest, ShippedButtonsArmsFromAnAxisRewriteAlone) {
  const std::string dir = Dir("buttons-axis");
  Install(dir, kButtonsPath);
  Settings settings;
  settings["buttons.axis.invert_lstick_x"] = "true";

  // 轴改写不进任何一张 vector：武装与否按内容判，这一句就够交出一个钩子。
  const auto hooks = Start(settings, MakeHost(), dir);
  ASSERT_EQ(hooks.size(), 1u);
  std::uint8_t state[kPluginStateSize] = {};
  state[0] = 200;
  state[1] = 128;
  state[2] = 128;
  state[3] = 128;
  hooks[0](state);
  EXPECT_EQ(state[0], 56);    // 256 - 200：静置点仍是静置点
  EXPECT_EQ(state[1], 128);   // 没被点名的那根轴一个字都不动
}

TEST_F(PluginTest, ShippedButtonsPowersOffOnTheHeartbeatNotTheHook) {
  const std::string dir = Dir("buttons-power-off");
  Install(dir, kButtonsPath);
  Settings settings;
  settings["buttons.shortcut.1"] = "create+options: power_off";

  const auto hooks = Start(settings, MakeHost(), dir);
  ASSERT_EQ(hooks.size(), 1u);

  std::uint8_t state[kPluginStateSize] = {};
  state[0] = state[1] = state[2] = state[3] = 128;  // 摇杆静置
  state[8] = 0x10 | 0x20;                           // Create + Options
  hooks[0](state);
  // K-10：命中只在这帧被记下来。钩子跑在 bt 的读线程上，它自己不碰控制通道。
  EXPECT_EQ(g_power_off_calls, 0);

  plugin::OnTick();
  EXPECT_EQ(g_power_off_calls, 1);
  // 取走即清空（K-11）：一次命中只发一份 0x08，发送失败也不会每轮撞一次链路。
  plugin::OnTick();
  EXPECT_EQ(g_power_off_calls, 1);

  // 那组键全松开之后再按一次，闸门才重新打开（K-08）。
  state[8] = 0;
  hooks[0](state);
  state[8] = 0x10 | 0x20;
  hooks[0](state);
  plugin::OnTick();
  EXPECT_EQ(g_power_off_calls, 2);
}

/**
 * @brief 把一个触摸点摆进状态体：线上那一个 32 位小端打包字。
 * @param state 状态体。
 * @param slot 第几个触点，线上就是 [32..35] 与 [36..39]。
 * @param x 12 位横坐标，0..4095。
 * @param y 12 位纵坐标，向下增长。
 * @param touching false 时报"未接触"。
 * @note 低 7 位是手柄自己的触点序号，bit7 才是接触标志：这里序号报 0，插件判半区
 *       与划动都不看它。
 */
void PutFinger(std::uint8_t* state, int slot, std::uint16_t x, std::uint16_t y,
               bool touching) {
  std::uint32_t word = (static_cast<std::uint32_t>(y) << 20) |
                       (static_cast<std::uint32_t>(x) << 8);
  if (!touching) word |= 1u << 7;
  for (int i = 0; i < 4; ++i) {
    state[32 + slot * 4 + i] = static_cast<std::uint8_t>(word >> (8 * i));
  }
}

TEST_F(PluginTest, ShippedButtonsRewritesAHalfClick) {
  const std::string dir = Dir("buttons-touchpad");
  Install(dir, kButtonsPath);
  Settings settings;
  settings["buttons.remap.pad_left"] = "cross";

  // 判据要从真实字节里读出来：这一条走的是 [32..35] 那两个坐标 → 半区 → 抹掉
  // [9] 的 pad 位，中间只隔着真实装载的那份 buttons.so。
  const auto hooks = Start(settings, MakeHost(), dir);
  ASSERT_EQ(hooks.size(), 1u);

  std::uint8_t state[kPluginStateSize] = {};
  state[7] = 0x08;   // 十字键未按。
  state[9] = 0x02;   // 触摸板按下。
  PutFinger(state, 0, 300, 2000, true);        // 落在左半。
  PutFinger(state, 1, 0, 0, false);            // 另一根手指不在面板上。
  hooks[0](state);
  EXPECT_EQ(state[7], 0x08 | 0x20);            // ✕ 出来了。
  EXPECT_EQ(state[9] & 0x02, 0u);              // pad 那一位一并抹掉。

  // 抬手：半区是落在那一刻锁存到手指上的，不清这一轮的话下一句手势还跟着上一句
  // 的起点。
  state[9] = 0;
  PutFinger(state, 0, 0, 0, false);
  hooks[0](state);

  // 同一份线上字节，手指落在右半：那条规则这一帧没被触发，pad 照原样交出去。
  state[7] = 0x08;
  state[9] = 0x02;
  PutFinger(state, 0, 3800, 2000, true);
  hooks[0](state);
  EXPECT_EQ(state[9] & 0x02, 0x02);
  EXPECT_EQ(state[7] & 0x20, 0u);
}

TEST_F(PluginTest, ShippedButtonsPowersOffOnASwipe) {
  const std::string dir = Dir("buttons-swipe");
  Install(dir, kButtonsPath);
  Settings settings;
  settings["buttons.shortcut.1"] = "swipe_up+touch_left: power_off";

  const auto hooks = Start(settings, MakeHost(), dir);
  ASSERT_EQ(hooks.size(), 1u);

  std::uint8_t state[kPluginStateSize] = {};
  for (int slot = 0; slot < 2; ++slot) PutFinger(state, slot, 0, 0, false);
  state[7] = 0x08;
  PutFinger(state, 0, 300, 2000, true);   // 落指在左半：这一帧只记下起点。
  hooks[0](state);
  EXPECT_EQ(g_power_off_calls, 0);

  PutFinger(state, 0, 300, 1000, true);   // 向上擦过 1000 个刻度：划动判出来了。
  hooks[0](state);
  EXPECT_EQ(g_power_off_calls, 0);        // 与组合键同一条相位：钩子不碰控制通道。
  plugin::OnTick();
  EXPECT_EQ(g_power_off_calls, 1);
}

#endif  // DS5DUMMY_TEST_BUTTONS_SO

}  // namespace
