/**
 * @file audio_identity_test.cpp
 * @brief audio::identity 的键、取值口径与节点名形状测试
 *
 * 只测字典写入与字符串拼装，不起 PipeWire、不连 daemon：`pw_properties_*` 是
 * 纯内存操作，所以这里能在无 root、无硬件的环境下钉住四件事——硬件身份的
 * 字符串形态（Wine 的 winepulse 按 16 进制读回 VID/PID，写成十进制就是另一个
 * 值）、`device.sysfs.path` 与由它推出的 `alsa.card` 的"空则不写"、两条流除
 * 图标外报同一份身份、以及节点名里那三段要被消费方 `strstr` 命中的子串。
 */

#include "audio/audio_identity.h"

#include <pipewire/pipewire.h>

#include <string>

#include <gtest/gtest.h>

namespace {

/**
 * @brief 取一项属性的字符串值，供逐字节比较。
 *
 * @param props 已建好的字典。
 * @param key   要读的键。
 * @return 属性指针；键不存在时为 null——null 与空串在断言里含义不同，那正是
 *         "缺键"和"写了空值"的差别。
 * @note 指针属于字典，调用方不得释放。
 */
const char* Get(const pw_properties* props, const char* key) {
  return pw_properties_get(props, key);
}

/**
 * @brief 断言一份已写好的字典带着完整硬件身份。
 *
 * 期望值写成字面量而不是复用生产常量：那样形态抄错（VID 少了 "0x" 前缀、
 * 十六进制写成大写）才会被看见。
 *
 * @param props 过完 ApplyHardwareIdentity 的字典。
 */
void ExpectSharedIdentity(const pw_properties* props) {
  EXPECT_STREQ(Get(props, PW_KEY_DEVICE_BUS), "usb");
  EXPECT_STREQ(Get(props, PW_KEY_DEVICE_VENDOR_ID), "0x054c");
  EXPECT_STREQ(Get(props, PW_KEY_DEVICE_PRODUCT_ID), "0x0ce6");
  EXPECT_STREQ(Get(props, PW_KEY_DEVICE_PRODUCT_NAME),
               "DualSense Wireless Controller");
  EXPECT_STREQ(Get(props, PW_KEY_DEVICE_CLASS), "sound");
  EXPECT_STREQ(Get(props, PW_KEY_DEVICE_FORM_FACTOR), "controller");
}

TEST(AudioIdentity, WritesHardwareIdentity) {
  pw_properties* props = pw_properties_new(nullptr, nullptr);
  ASSERT_NE(props, nullptr);

  audio::identity::ApplyHardwareIdentity(
      props, audio::identity::kSinkIconName,
      "/devices/platform/dummy_hcd.0/usb9/9-1/9-1:1.0");

  ExpectSharedIdentity(props);
  EXPECT_STREQ(Get(props, PW_KEY_DEVICE_ICON_NAME), "audio-card-analog");
  pw_properties_free(props);
}

TEST(AudioIdentity, WritesSysfsPathOnlyWhenPresent) {
  pw_properties* props = pw_properties_new(nullptr, nullptr);
  ASSERT_NE(props, nullptr);

  // Manager 建站时没等到主机把设备枚举出来（或只等到设备目录、没等到它下面的
  // 接口目录）：这一站就没有路径可报，属性索性不出现，而不是带一个空值。
  audio::identity::ApplyHardwareIdentity(props, audio::identity::kSinkIconName,
                                         "");
  EXPECT_EQ(Get(props, PW_KEY_DEVICE_SYSFS_PATH), nullptr);

  audio::identity::ApplyHardwareIdentity(
      props, audio::identity::kSinkIconName,
      "/devices/platform/dummy_hcd.3/usb12/12-1/12-1:1.0");
  EXPECT_STREQ(Get(props, PW_KEY_DEVICE_SYSFS_PATH),
               "/devices/platform/dummy_hcd.3/usb12/12-1/12-1:1.0");
  pw_properties_free(props);
}

TEST(AudioIdentity, BothStreamsShareOneIdentityAndDifferOnlyInIcon) {
  pw_properties* sink = pw_properties_new(nullptr, nullptr);
  pw_properties* source = pw_properties_new(nullptr, nullptr);
  ASSERT_NE(sink, nullptr);
  ASSERT_NE(source, nullptr);

  constexpr char kPath[] = "/devices/platform/dummy_hcd.0/usb9/9-1/9-1:1.0";
  audio::identity::ApplyHardwareIdentity(sink, audio::identity::kSinkIconName,
                                         kPath);
  audio::identity::ApplyHardwareIdentity(
      source, audio::identity::kSourceIconName, kPath);

  // 同一台 USB 声卡的两条流：身份不一致时按硬件查找端点的消费者只认其中
  // 一条，所以除了图标，两条流必须逐键同值。
  ExpectSharedIdentity(source);
  EXPECT_STREQ(Get(sink, PW_KEY_DEVICE_SYSFS_PATH),
               Get(source, PW_KEY_DEVICE_SYSFS_PATH));
  EXPECT_STREQ(Get(sink, PW_KEY_DEVICE_ICON_NAME), "audio-card-analog");
  EXPECT_STREQ(Get(source, PW_KEY_DEVICE_ICON_NAME), "audio-input-microphone");

  pw_properties_free(sink);
  pw_properties_free(source);
}

TEST(AudioIdentity, NullDictionaryIsIgnored) {
  // 调用方的字典没建起来时不能崩：那份错误由调用方自己的建流分支报，这里
  // 只确认写操作本身不越界。
  audio::identity::ApplyHardwareIdentity(
      nullptr, audio::identity::kSinkIconName,
      "/devices/platform/dummy_hcd.0/usb9/9-1/9-1:1.0");
  SUCCEED();
}

TEST(AudioIdentity, WritesBothAlsaCardSpellingsFromTheAdapterIndex) {
  pw_properties* props = pw_properties_new(nullptr, nullptr);
  ASSERT_NE(props, nullptr);

  audio::identity::ApplyHardwareIdentity(
      props, audio::identity::kSinkIconName,
      "/devices/platform/dummy_hcd.3/usb12/12-1/12-1:1.0");

  // 读方是"alsa.card，读不到再退 api.alsa.card"，所以两个键必须同值；序号 3
  // 来自路径里的适配器名，不是 ALSA 真卡号。
  EXPECT_STREQ(Get(props, "alsa.card"), "3");
  EXPECT_STREQ(Get(props, "api.alsa.card"), "3");
  pw_properties_free(props);
}

TEST(AudioIdentity, WritesNoAlsaCardWhenThePathIsUnusable) {
  pw_properties* props = pw_properties_new(nullptr, nullptr);
  ASSERT_NE(props, nullptr);

  // 建站没等到枚举：整段路径为空，两个键都不出现。
  audio::identity::ApplyHardwareIdentity(props,
                                         audio::identity::kSinkIconName, "");
  EXPECT_EQ(Get(props, "alsa.card"), nullptr);

  // 认不出适配器名的形状（不是 dummy_hcd 前缀，或序号后紧跟的不是分隔符）：
  // 宁可不写，也不交出一个猜出来的 0——0 是合法序号，写进去会被读方当成第一号
  // 控制器参与排序。
  audio::identity::ApplyHardwareIdentity(
      props, audio::identity::kSinkIconName, "/devices/platform/usb1/1-1");
  EXPECT_EQ(Get(props, "alsa.card"), nullptr);
  EXPECT_EQ(Get(props, "api.alsa.card"), nullptr);

  audio::identity::ApplyHardwareIdentity(
      props, audio::identity::kSinkIconName,
      "/devices/platform/dummy_hcd.0ab");
  EXPECT_EQ(Get(props, "alsa.card"), nullptr);
  pw_properties_free(props);
}

TEST(AudioIdentity, SinkNodeNameCarriesTheConsumerSubstrings) {
  const std::string name =
      audio::identity::MakeSinkNodeName("0c275682705e");

  // 这三段是 GE-Proton 的 winepulse 判"这只手柄的喇叭 sink"时的 strstr 条件，
  // 少一段节点就退回普通设备；字面量抄在这里而不是复用生产常量，改了形状
  // 会被这里拦下。
  EXPECT_NE(name.find("alsa_output.usb-Sony_Interactive_Entertainment_"),
            std::string::npos);
  EXPECT_NE(name.find("Wireless_Controller"), std::string::npos);
  EXPECT_NE(name.find("Speaker__sink"), std::string::npos);

  // 完整形状同时钉住：本站标识占的是真机名字里 USB function 号（`-00`）的位置。
  EXPECT_EQ(name,
            "alsa_output.usb-Sony_Interactive_Entertainment_DualSense_"
            "Wireless_Controller-0c275682705e.HiFi__Speaker__sink");
}

TEST(AudioIdentity, SourceNodeNameIsTheSameFamilyWithItsOwnProfile) {
  constexpr char kStation[] = "0c275682705e";
  // 采集侧没有已知的名字判据（winepulse 认麦克风靠硬件身份那组键加方向），
  // 这里钉的是形状：与 sink 同一段设备名，只换前后两段。
  EXPECT_EQ(audio::identity::MakeSourceNodeName(kStation),
            "alsa_input.usb-Sony_Interactive_Entertainment_DualSense_"
            "Wireless_Controller-0c275682705e.HiFi__Mic__source");
  EXPECT_NE(audio::identity::MakeSinkNodeName(kStation)
                .find("usb-Sony_Interactive_Entertainment_DualSense_"
                      "Wireless_Controller-0c275682705e."),
            std::string::npos);
}

TEST(AudioIdentity, StationIdsDistinguishTwoStations) {
  // 多站并发时 node.name 必须互不相同：撞名会让第二站被看成第一站的重连。
  EXPECT_NE(audio::identity::MakeSinkNodeName("0c275682705e"),
            audio::identity::MakeSinkNodeName("0c275682705f"));
  EXPECT_EQ(audio::identity::MakeDeviceId("0c275682705e"),
            "USB054c:0ce6.0c275682705e");
}

}  // namespace
