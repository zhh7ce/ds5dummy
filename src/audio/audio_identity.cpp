/**
 * @file audio_identity.cpp
 * @brief audio::identity 实现：本站虚拟 USB 声卡对主机侧消费者报出的身份
 *
 * 只做两件事：往属性字典里写"这台设备是谁"那一组键，以及按消费方会匹配的
 * 形状拼节点名。不碰 PipeWire 连接、不管采样格式与生命周期。键与值的口径
 * 对齐同一只控制器在有线模式（真 UAC1 声卡）下 ALSA 节点上的实测属性，节点
 * 名的口径对齐同一模式下 WirePlumber UCM 切出来的 sink 名——缺了这两样，
 * 按硬件身份查找音频端点的消费者（Wine 的 winepulse、GE-Proton 的手柄补丁）
 * 就不会把这个节点当成那只手柄自己的声卡。
 *
 * 真节点上的 `alsa.*` 族只搬了卡号那两项（消费方点名要它），`device.api` 与
 * `device.profile.*` 仍然跳过，理由写在 ApplyHardwareIdentity 的末尾。
 */

#include "audio/audio_identity.h"

#include <charconv>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "core/device/identity.h"

namespace audio {
namespace identity {
namespace {

/// device.bus 的取值：PipeWire 只认 "isa"/"pci"/"usb"/"firewire"/"bluetooth"
/// 这一组字面量，Wine 的 winepulse 对它是精确 strcmp，写错了就等于没写。
constexpr char kDeviceBusUsb[] = "usb";

/// device.class 的取值：真设备上是 alsa-udev 按声卡类别填的 "sound"。Wine
/// 只在节点描述超长时拿它和 "monitor" 比，其余消费者按类别分组设备。
constexpr char kDeviceClassSound[] = "sound";

/// device.form-factor 的取值：真手柄的 ALSA 节点报 "controller"，不是通用
/// 声卡的 "internal"。这个键是"这台设备是手柄"在属性层的表述。
constexpr char kDeviceFormFactor[] = "controller";

/// alsa.card 的键名：真 ALSA 播放/采集节点上由 alsa-udev 填的卡索引。GE-Proton
/// 的 winepulse 读 "alsa.card"，读不到再退 "api.alsa.card"，两个键取同值。
constexpr char kAlsaCard[] = "alsa.card";

/// api.alsa.card 的键名：上面那条回落分支。见 kAlsaCard。
constexpr char kApiAlsaCard[] = "api.alsa.card";

// 节点名的两端：中间空出来的那一段放本站标识，位置对应真机名字里的 USB
// function 号（`-00`）。三段内容合起来是消费方的判断量，见 MakeSinkNodeName
// 的注释——前缀里的 `alsa_output.usb-Sony_Interactive_Entertainment_` 与
// `Wireless_Controller` 必须同时出现，缺一段那条判据就不成立。
constexpr char kSinkNamePrefix[] =
    "alsa_output.usb-Sony_Interactive_Entertainment_DualSense_Wireless_"
    "Controller-";
constexpr char kSinkNameSuffix[] = ".HiFi__Speaker__sink";
constexpr char kSourceNamePrefix[] =
    "alsa_input.usb-Sony_Interactive_Entertainment_DualSense_Wireless_"
    "Controller-";
constexpr char kSourceNameSuffix[] = ".HiFi__Mic__source";

/**
 * @brief 把 USB VID/PID 数值写成 device.vendor.id / device.product.id 的约定形态。
 *
 * 真设备上的这个值是 alsa-udev 按 "0x" + 4 位小写十六进制填的（例如
 * "0x1022"），PipeWire 的 pulse 协议层把节点属性原样序列化进 PulseAudio 的
 * sink proplist，Wine 的 winepulse 再按 16 进制把它读回去拼进设备实例路径
 * `USB\VID_%04X&PID_%04X`——所以必须保持同一形态，十进制串会被当成另一个值。
 *
 * @param id VID 或 PID 数值（16 位）。
 * @return 形如 "0x054c" 的字符串，长度恒为 6。
 * @note 纯函数，无副作用。
 */
std::string FormatHardwareId(uint16_t id) {
  return std::format("0x{:04x}", id);
}

/**
 * @brief 从主机侧设备目录里取出本站虚拟主控制器（dummy_hcd）的序号。
 *
 * 这个序号是本项目的"卡号"：每站绑一个 `dummy_hcd.N`，同一主机内唯一且按站
 * 稳定，与真 ALSA 卡索引在"同一主机内的声卡编号"这个含义上同构。
 *
 * @param host_sysfs_path 相对 /sys 的接口目录（设备目录的下级），形如
 *        `/devices/platform/dummy_hcd.0/usb7/7-1/7-1:1.0`；空串与不含
 *        `dummy_hcd.` 的串都算解析失败。
 * @return 序号；任何一步不合形状返回 std::nullopt，调用方据此**不写**卡号键，
 *         而不是写一个失败的 0（0 是合法序号，写进去会被读方当成第一号适配器）。
 * @note 纯函数，无副作用，只做字符串扫描。
 */
std::optional<int> ParseAdapterIndex(const std::string& host_sysfs_path) {
  static constexpr std::string_view kMarker = "dummy_hcd.";
  const size_t begin = host_sysfs_path.find(kMarker);
  if (begin == std::string::npos) return std::nullopt;

  const size_t digits = begin + kMarker.size();
  const char* const end = host_sysfs_path.data() + host_sysfs_path.size();
  int index = 0;
  const std::from_chars_result parsed =
      std::from_chars(host_sysfs_path.data() + digits, end, index);
  // 必须真吃到至少一位数字，且序号整段就是目录名（后面紧跟 '/' 或到串尾）：
  // 否则 "dummy_hcd.0abc" 这种畸形串会静默交出 0。
  if (parsed.ec != std::errc{} || parsed.ptr == host_sysfs_path.data() + digits)
    return std::nullopt;
  if (parsed.ptr != end && *parsed.ptr != '/') return std::nullopt;

  return index;
}

}  // namespace

void ApplyHardwareIdentity(pw_properties* props, const char* icon_name,
                           const std::string& host_sysfs_path) {
  if (!props) return;  // 调用方的字典没建起来：这里不做第二层报错。

  const std::string vendor_id = FormatHardwareId(core::device::kSonyVid);
  const std::string product_id = FormatHardwareId(core::device::kDs5Pid);

  // VID/PID/产品名取自 core::device 的编译期常量，与 gadget 描述符报给主机的
  // 那一同源：这里的字符串一旦和描述符不一致，按硬件身份找音频端点的消费者
  // 就对不上号了。
  pw_properties_set(props, PW_KEY_DEVICE_BUS, kDeviceBusUsb);
  pw_properties_set(props, PW_KEY_DEVICE_VENDOR_ID, vendor_id.c_str());
  pw_properties_set(props, PW_KEY_DEVICE_PRODUCT_ID, product_id.c_str());
  pw_properties_set(props, PW_KEY_DEVICE_PRODUCT_NAME, core::device::kProduct);
  pw_properties_set(props, PW_KEY_DEVICE_ICON_NAME, icon_name);
  pw_properties_set(props, PW_KEY_DEVICE_CLASS, kDeviceClassSound);
  pw_properties_set(props, PW_KEY_DEVICE_FORM_FACTOR, kDeviceFormFactor);

  // 路径按"有就写"处理：Manager 建站时可能没等到主机把设备枚举出来，那种情况
  // 下两个键都不写。空值会让按 sysfs 反查设备目录的消费者算出空路径，反而不如
  // 缺键。
  if (!host_sysfs_path.empty()) {
    pw_properties_set(props, PW_KEY_DEVICE_SYSFS_PATH,
                      host_sysfs_path.c_str());
  }

  // 卡号是从同一段路径现推的，不是第二个来源：读它的那处代码（GE-Proton
  // 0189）拿它给同型号两只手柄定 Canonical 身份，取号小的那只，所以单站时无
  // 所谓、多站时它必须互不相同。两个键写同一个字符串，因为读方是"或"的关系。
  if (const std::optional<int> adapter = ParseAdapterIndex(host_sysfs_path)) {
    const std::string card = std::to_string(*adapter);
    pw_properties_set(props, kAlsaCard, card.c_str());
    pw_properties_set(props, kApiAlsaCard, card.c_str());
  }

  // 刻意不搬的两组键。device.api=alsa：一旦写上，WirePlumber 的 ALSA 规则就会
  // 把它当真实声卡节点、套上按卡索引生效的路由逻辑，而我们背后没有 ALSA 设备
  // 可路由。其余 alsa.*（alsa.card_name/pcm.name/sync.id…）也没有对应的真实
  // 声卡可指。device.profile.name/description：唯一的读方是 Wine 拼设备名的
  // 那一段（winepulse.drv/pulse.c 的 get_device_name），它只在
  // node.description 超过 62 字符时才会把 profile.description 追加到
  // product.name 后面。采集节点的描述确实超了（72 字符），所以它要的正是
  // product.name 这一项——本函数已经写上；profile 那半份真机字面量我们手上
  // 没有实测来源，宁可让 Wine 只报产品名，也不编一个。
}

std::string MakeSinkNodeName(const std::string& station_id) {
  return std::string(kSinkNamePrefix) + station_id + kSinkNameSuffix;
}

std::string MakeSourceNodeName(const std::string& station_id) {
  return std::string(kSourceNamePrefix) + station_id + kSourceNameSuffix;
}

}  // namespace identity
}  // namespace audio
