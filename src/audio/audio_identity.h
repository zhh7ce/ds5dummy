/**
 * @file audio_identity.h
 * @brief audio::identity —— 虚拟声卡对主机侧消费者报出的身份
 *
 * 职责是"这份身份长什么样"，两件事：往一个已建好的 `pw_properties` 字典里写
 * "这台设备是谁"那一组键，以及按消费方实际会匹配的形态拼出 ALSA 风格的节点
 * 名。采样格式、生命周期、`media.role` 都不在这里——那些各自属于 audio::sink
 * 与 audio::source。单独成模块的理由是同一只手柄在主机上就是**一台** USB 声卡，
 * 播放 sink 与录音 source 必须报出同一份硬件身份、名字也必须同形；两处各写
 * 一遍迟早会漂，而漂移的后果是按硬件身份找端点的消费者只认其中一条流。
 *
 * 值的来源分三类：VID/PID/产品名取自 `core::device::identity.h` 的编译期
 * 常量，与 gadget 描述符报给主机的那一对同源；`device.icon-name` 等几项是
 * 同一只控制器在有线模式（真 UAC1 声卡）下 ALSA 播放节点上的实测字面量，
 * 原样搬过来，只有图标按流的方向分岔；节点名的形状则照同一模式下 WirePlumber
 * UCM 切出来的 `alsa_output.usb-…__Speaker__sink` 族，因为消费方（GE-Proton
 * 的 winepulse）判据里就有对这些子串的 `strstr`。
 */

#pragma once

#include <pipewire/pipewire.h>

#include <string>

namespace audio {
namespace identity {

/// 播放节点的 XDG 图标名：真手柄有线模式下报的就是这个值，不是通用声卡的
/// "audio-card"。
constexpr char kSinkIconName[] = "audio-card-analog";

/// 录音节点的图标名：手上有实测来源的只有有线模式的播放节点，采集节点没有
/// 对应字面量可抄，这里沿用 PulseAudio 对采集设备的通行写法。
constexpr char kSourceIconName[] = "audio-input-microphone";

/**
 * @brief 拼播放 sink 的节点名，形态照真手柄有线模式下 UCM 切出的喇叭 sink。
 *
 * 这个字符串不是给人看的标签，是被 `strstr` 读的判断量：GE-Proton 的
 * winepulse 把手柄的喇叭 sink 认成"这只控制器的扬声器"，靠的是节点名里同时
 * 出现 `alsa_output.usb-Sony_Interactive_Entertainment_`、
 * `Wireless_Controller` 与 `Speaker__sink` 三段（`alsa_output.usb-…` 与
 * `…Wireless_Controller` 那两条要一起满足，缺一段就落回普通设备）。名字里
 * 唯一自由的一段是本站的 MAC 十六进制，占的是真机上 USB function 号 `-00`
 * 的位置——两站同时在线时靠它保证 `node.name` 不撞车。
 *
 * @param station_id 本站标识：12 位小写 MAC 十六进制，调用方负责形状。
 * @return 完整的节点名，形如
 *         `alsa_output.usb-Sony_Interactive_Entertainment_DualSense_Wireless_Controller-0c275682705e.HiFi__Speaker__sink`。
 * @note 纯函数，无副作用；改动返回值形状前先把上面三段 `strstr` 判据当门禁
 *       重新核一遍（tests/audio_identity_test.cpp 逐条钉住了它们）。
 * @see MakeSourceNodeName —— 同形状的采集侧。
 */
std::string MakeSinkNodeName(const std::string& station_id);

/**
 * @brief 拼录音 source 的节点名，与 MakeSinkNodeName 同一族前缀。
 *
 * 采集侧没有任何已知的名字判据（winepulse 认麦克风靠的是硬件身份那组键加
 * `eCapture` 方向），这里的形状只为让两条流在 `pw-dump` 里读起来是同一台
 * 声卡的播放与录音两半边。
 *
 * @param station_id 本站标识，与 sink 侧同一个值。
 * @return 形如 `alsa_input.usb-Sony_…_Wireless_Controller-0c275682705e.HiFi__Mic__source`。
 * @note 纯函数，无副作用。
 * @see MakeSinkNodeName。
 */
std::string MakeSourceNodeName(const std::string& station_id);

/**
 * @brief 把本站虚拟 USB 声卡的硬件身份写进节点属性字典。
 *
 * PipeWire 的 pulse 协议层把节点字典**全量**序列化给 Pulse 客户端
 * （`fill_sink_info_proplist()` 直接把节点 props 打上 TAG_PROPLIST，不做键
 * 过滤），libpulse 的 `pa_proplist_key_valid()` 也只校验 ASCII 与长度，所以
 * 这里的每一个键客户端都能按名字读到。真实 ALSA 声卡这几项挂在
 * `Audio/Device` 对象上、由同一层合并进 sink 字典；我们的流节点没有 Device，
 * 只能直接挂在节点上，效果等价。
 *
 * @param props 已建好的属性字典，本函数只 set 自己那几项，不接管所有权，
 *              也不覆盖调用方写过的键。
 * @param icon_name `device.icon-name` 的取值，按流的方向选
 *                  kSinkIconName/kSourceIconName。
 * @param host_sysfs_path 本站在主机侧的 USB 接口目录，相对 /sys，形如
 *                  `/devices/platform/dummy_hcd.0/usb9/9-1/9-1:1.0`；空串表示
 *                  Manager 建站时没等到枚举，此时**不写** `device.sysfs.path`
 *                  ——留着空值会让按 sysfs 路径反查 udev 的消费者算出空结果，
 *                  比缺键更糟。它必须是设备目录**下面**的一级：这类消费者沿
 *                  父链找 `usb_device` 祖先，而设备目录自己就是那个祖先，父链
 *                  会跳过它去命中 root hub（见 usb::detail::
 *                  FindHostInterfaceDir）。同一段路径里的 `dummy_hcd.<N>` 序号
 *                  会被写成 `alsa.card`/`api.alsa.card`（解析不出来就不写）。
 * @note 只改内存里的字典，不碰 PipeWire 也不分配 I/O；调用方在建流之前调用。
 *       本函数只管与方向无关的那一组：`device.*` 加两个同值的 ALSA 卡号键。
 *       `media.role`、量子类提示与 `node.pause-on-idle` 留在各自调用点，
 *       节点名由 MakeSinkNodeName/MakeSourceNodeName 单独负责。
 * @see audio::sink::Start、audio::source::Start —— 两个调用点各自传入自己的
 *      图标名与 sysfs 路径。
 */
void ApplyHardwareIdentity(pw_properties* props, const char* icon_name,
                           const std::string& host_sysfs_path);

}  // namespace identity
}  // namespace audio
