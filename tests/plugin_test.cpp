/**
 * @file
 * @brief plugin 模块的单测：目录扫描、标记识别、装载与分派（无需 root/手柄）
 *
 * 覆盖的是"插件目录里一个 .so 能不能变成一个会跑回调的插件"这条链路：
 * - 目录扫描：只认 `.so` 普通文件，不存在/为空的目录不影响启动；
 * - 标记识别：没有 `.ds5_plugin` 节的 .so 在 dlopen 之前就被跳过；
 * - dlopen 路径：真加载测试期间构建出来的 .so，并证明它的钩子真的被调用过；
 * - 同名去重与目录优先级、版本号不符的 .so 在装载点被拒；
 * - 随包 idle 的真实产物从目录里被装载；
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

}  // namespace
