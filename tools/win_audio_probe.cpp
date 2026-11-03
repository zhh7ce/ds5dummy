/**
 * @file win_audio_probe.cpp
 * @brief Windows 侧音频环境探针：把游戏进程真正能看到的东西原样打出来
 *
 * 职责：在 Proton/Wine 前缀里运行，从 Win32 API 这一侧枚举 MMDevice 端点、读端点的
 * 属性存储、读 setupapi 里的设备接口与容器 id、取共享模式的混音格式，并可选地往指定
 * 端点的指定通道灌一段正弦。它是本项目 audio 身份伪装那一层的"读方"证据来源：
 * `pactl`/`pw-dump` 只能证明我们把哪些键写进了 PipeWire 节点字典，这一层才证明
 * winepulse 那侧的补丁把这些键翻译成了什么样的 Windows 设备身份。
 *
 * 不做什么：不做任何判断、不比较期望值、不写文件、不装回调、不常驻——跑一遍就退出。
 * 判读由人对着输出做，因为"对"的标准在补丁树里，会随 GE-Proton 版本变。
 *
 * 层次：tools/ 下的诊断程序，与 mini_gadget 同类——故意绕开 src/ 的门面边界，不链
 * 任何项目代码，只用 Win32/COM API。因此它不参与 Linux 侧构建，交叉编译命令写在下面。
 *
 * 编译（Linux 主机，mingw-w64 交叉工具链）：
 *   x86_64-w64-mingw32-g++ -std=c++20 -O1 -Wall -Wextra -municode -static \
 *     -o win_audio_probe.exe tools/win_audio_probe.cpp \
 *     -lole32 -loleaut32 -lsetupapi -luuid -lksuser
 * `-static` 把 libstdc++/libgcc 链进来：exe 要单独扔进 Proton 前缀，前缀里没有
 * mingw 的运行时 DLL。`-lksuser` 提供 `KSCATEGORY_*` 与 `KSDATAFORMAT_SUBTYPE_*`
 * 这批 GUID 的定义（mingw 把它们放在同名库里，和真机的 ksuser.lib 同源）。
 * 运行：
 *   WINEDEBUG=-all wine ./win_audio_probe.exe                       # 只读快照
 *   WINEDEBUG=-all wine ./win_audio_probe.exe --write "<id 子串>" --ch 4 --secs 3
 * `--write` 的子串直接抄枚举里 `[n] {0.0.0.00000000}.{GUID}` 那行，带 GUID 就够了；
 * `--ch` 是 1 起的通道号，只有该通道有能量，其余通道写 0。
 * 在 Proton 前缀里则用 `proton run win_audio_probe.exe`，`STEAM_COMPAT_DATA_PATH` 与
 * `STEAM_COMPAT_CLIENT_INSTALL_PATH` 指向目标前缀。
 *
 * 线程模型：单线程，STA（`COINIT_APARTMENTTHREADED`）——和用 event-driven 渲染的游戏
 * 进程同一形态，`--write` 的推送循环也在这条线程上用 `Sleep` 让出。
 * 编码：所有输出先转成 UTF-8 窄串再写 stdout，避免依赖控制台代码页。
 */

#include <windows.h>
#include <initguid.h>
#include <basetyps.h>
#include <devpkey.h>
#include <devguid.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <hidclass.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <setupapi.h>
#include <wtypes.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

/// `--write` 默认灌多久（秒）：够 PipeWire 侧的 worker 采一个 5 s 统计窗口的一多半。
constexpr int kDefaultWriteSeconds = 3;

/// `--write` 默认灌哪个通道（1 起）：通道 3/4 才是触觉/特殊音效那两路。
constexpr int kDefaultWriteChannel = 4;

/// 正弦频率（Hz）：经验值，人耳好认、48 kHz 下周期整数化不重要（只看有没有能量）。
constexpr double kToneFrequencyHz = 440.0;

/// 每 10 ms 推一次缓冲：与 WASAPI 共享模式常见的 packet 粒度同阶，够喂满又不忙等。
constexpr int kWriteTickMs = 10;

/// blob 属性最多打前多少个字节：混音格式之类的 blob 头部就够判读，全打会刷屏。
constexpr size_t kMaxBlobBytes = 24;

/**
 * `VT_VECTOR | VT_LPWSTR` 的数值 = 0x101f；mingw 的 VARTYPE 枚举里没有这个组合名
 * （只有分开的 VT_VECTOR 与 VT_LPWSTR），`case` 标签必须是编译期常量，所以在此拼好。
 */
constexpr VARTYPE kVtVectorLpWstr =
    static_cast<VARTYPE>(VT_VECTOR | VT_LPWSTR);

/**
 * Wine 内部属性键 `{B3F8FA53-…}-2`（SDK 里没有名字，`devenum.c` 叫它
 * `devicepath_key`）：winepulse 在这里交出设备实例路径，值是
 * `{1}.USB\VID_054C&PID_0CE6\<PA 索引>&<端点 GUID 尾 8 位>` 这类串。
 * `PKEY_Device_InstanceId` 在 Wine 上恒为空，所以判读实例身份只能看这一项。
 */
constexpr PROPERTYKEY kWineDevicePathKey = {
    {0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}},
    2};

/**
 * @brief 往 stdout 写一行 UTF-8 文本。
 *
 * @param line 不含行尾换行的窄串（调用方负责已转成 UTF-8）。
 * @note 无返回值：探针不报告输出失败，写不出去就当没发生。
 */
void Print(const std::string& line) {
  std::fputs(line.c_str(), stdout);
  std::fputc('\n', stdout);
  // 逐行冲：程序可能在 COM 调用里死掉，行缓冲会把已经打出来的证据一起带走。
  std::fflush(stdout);
}

/**
 * @brief 把宽字符串转成 UTF-8 窄串，供 Print 使用。
 *
 * @param text 宽字符串，允许为 null。
 * @return 转换后的 UTF-8 串；`text` 为 null 时返回空串。
 * @note 转换失败（非法代理对等）返回空串，不返回半截内容——空串在输出里的含义是
 *       "这一项取不到"，与真取到空值同形，判读时看它上面的键名行。
 */
std::string ToUtf8(LPCWSTR text) {
  if (!text) return "";
  const int need = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr,
                                       nullptr);
  if (need <= 0) return "";
  std::string out(static_cast<size_t>(need) - 1, '\0');
  // -1 长度表示以 null 结尾，返回值含终止符，所以扣 1 后正好写满 out。
  if (WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), need, nullptr,
                          nullptr) == 0) {
    return "";
  }
  return out;
}

/**
 * @brief 把 GUID 打成 Windows 习惯的 `{8-4-4-4-12}` 大写形态。
 *
 * @param guid 要打的 GUID，按值取（调用方可以传 `*puuid`）。
 * @return 形如 `{00000000-0000-0000-0000-000000000000}` 的串，长度恒为 38。
 * @note 统一大写（`StringFromGUID2` 的输出形态）：字符串比较是大小写敏感的，判读
 *       容器 id 那几行时两边必须同一口径。
 */
std::string GuidToString(const GUID& guid) {
  OLECHAR buf[64] = {0};
  if (StringFromGUID2(guid, buf, static_cast<int>(std::size(buf))) == 0) {
    return "<StringFromGUID2 failed>";
  }
  return ToUtf8(buf);
}

/**
 * @brief 把 PROPERTYKEY 打成 `{FMTID}-pid` 形式。
 *
 * @param key 属性键。
 * @return 供人对照 SDK 头文件的字符串。
 * @note 纯函数，无副作用。
 */
std::string PropertyKeyToString(const PROPERTYKEY& key) {
  return GuidToString(key.fmtid) + "-" + std::to_string(key.pid);
}

/**
 * @brief 把 KSAUDIO_SPEAKER 通道掩码翻成通道名列表。
 *
 * @param mask `WAVEFORMATEXTENSIBLE::dwChannelMask`。
 * @return 形如 "FL+FR+BL+BR" 的串；掩码为 0 时打 "<mask 0>"。
 * @note 位序与名字照 Windows 的 SPEAKER_* 定义（0..17 有效，18..30 是保留位，
 *       31 是 SPEAKER_ALL）：`KSAUDIO_SPEAKER_QUAD` = FL+FR+BL+BR = 0x33，
 *       也就是 Pulse 侧 `rear-left/rear-right` 那两路在 Windows 上的叫法。
 */
std::string ChannelMaskToString(DWORD mask) {
  static constexpr const char* kNames[] = {
      "FL",  "FR",  "FC", "LFE", "BL",  "BR",  "FLC", "FRC", "BC",
      "SL",  "SR",  "TC", "TFL", "TFC", "TFR", "TBL", "TBC", "TBR"};
  if (mask == 0) return "<mask 0>";
  std::string out;
  for (size_t bit = 0; bit < std::size(kNames); ++bit) {
    if (mask & (1UL << bit)) {
      if (!out.empty()) out += "+";
      out += kNames[bit];
    }
  }
  // 高位（18..31）不是合法通道位：真出现就说明上游构造掩码时用错了口径。
  if (mask & 0xFFFC0000UL) out += "+<高位 0x" + [&] {
    char buf[9] = {0};
    std::snprintf(buf, sizeof(buf), "%x", static_cast<unsigned>(mask & 0xFFFC0000UL));
    return std::string(buf);
  }();
  return out;
}

/**
 * @brief 把一段 `WAVEFORMATEX`/`WAVEFORMATEXTENSIBLE` 字节解成一行可读格式。
 *
 * @param data 属性 blob 的原始字节。
 * @param size 字节数；小于 `sizeof(WAVEFORMATEX)` 时无法解码。
 * @return 形如 `4ch/48000Hz/32bit 掩码 FL+FR+BL+BR subformat {...}` 的串；不能解码时
 *         返回空串（调用方据此只打十六进制）。
 * @note `PKEY_AudioEngine_DeviceFormat` 等处存的就是这个结构，长度 18（PCM）或 40
 *       （EXTENSIBLE）。EXTENSIBLE 才带通道掩码与子格式 GUID，这两项是判读
 *       "winepulse 把这台设备认成几声道的什么格式"的全部依据。
 */
std::string FormatWaveFormat(const BYTE* data, size_t size) {
  if (!data || size < sizeof(WAVEFORMATEX)) return "";
  WAVEFORMATEX fmt{};
  std::memcpy(&fmt, data, sizeof(fmt));
  std::string out = std::to_string(fmt.nChannels) + "ch/" +
                    std::to_string(fmt.nSamplesPerSec) + "Hz/" +
                    std::to_string(fmt.wBitsPerSample) + "bit" +
                    " blockAlign=" + std::to_string(fmt.nBlockAlign);
  // 0xFFFE = WAVE_FORMAT_EXTENSIBLE：只有这种形态在 WAVEFORMATEX 之后还有掩码与
  // 子格式，长度不足时按普通 PCM 处理，不越界读。
  if (fmt.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
      size >= sizeof(WAVEFORMATEXTENSIBLE)) {
    WAVEFORMATEXTENSIBLE ext{};
    std::memcpy(&ext, data, sizeof(ext));
    out += " 掩码 " + ChannelMaskToString(ext.dwChannelMask) + " 有效位 " +
           std::to_string(ext.Samples.wValidBitsPerSample) + " subformat " +
           GuidToString(ext.SubFormat);
  }
  return out;
}

/**
 * @brief 判断一个波形格式的采样类型是不是 32 位 float。
 *
 * @param fmt 混音格式或请求格式；调用方保证非 null。
 * @return true = 送缓冲时要写 float 样本。
 * @note 两种合法形态都要认：普通 `WAVE_FORMAT_IEEE_FLOAT`，以及
 *       `WAVE_FORMAT_EXTENSIBLE` + `KSDATAFORMAT_SUBTYPE_IEEE_FLOAT` 子格式——本项目
 *       4 通道 sink 报出来的就是后者，只看 `wFormatTag` 会把它误判成整形。
 */
bool IsFloat32Format(const WAVEFORMATEX* fmt) {
  if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    return fmt->wBitsPerSample == 32;
  }
  if (fmt->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
      fmt->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
    return false;
  }
  const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
  return fmt->wBitsPerSample == 32 &&
         IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
}

/**
 * @brief 按 VARTYPE 把 PROPVARIANT 的值打成可读文本。
 *
 * @param var 属性值，调用方从 `IPropertyStore::Get` 取回。
 * @return 该值的文本形态；不认识的类型打 `<VT 0xXXXX 未译>`，不猜内容。
 * @note 只读，不释放任何东西（blob/CLSID 的内存属于属性存储，`PropVariantClear`
 *       由持有者路径负责，这里不调）。
 */
std::string FormatPropVariant(const PROPVARIANT& var) {
  switch (var.vt) {
    case VT_EMPTY:
      return "<empty>";
    case VT_LPWSTR:
      return "\"" + ToUtf8(var.pwszVal) + "\"";
    case VT_BSTR:
      return "\"" + ToUtf8(var.bstrVal) + "\"";
    case VT_UI4:
      return std::to_string(var.ulVal);
    case VT_I4:
      return std::to_string(var.lVal);
    case VT_BOOL:
      return var.boolVal ? "true" : "false";
    case VT_CLSID:
      // 属性存储里 GUID 类的值（容器 id、子格式 id）用的就是 VT_CLSID = 0x48；
      // SDK 的另一个名字 VT_UUID 同值，mingw 头里没有它。存的是指针，null 指针
      // 既可能是"这项没有值"也可能是坏数据，两种都原样打出来。
      return var.puuid ? GuidToString(*var.puuid) : "<null GUID ptr>";
    case kVtVectorLpWstr: {
      std::string out = "[";
      for (ULONG i = 0; i < var.calpwstr.cElems; ++i) {
        if (i) out += ", ";
        out += ToUtf8(var.calpwstr.pElems[i]);
      }
      return out + "]";
    }
    case VT_BLOB: {
      // blob 是格式类属性的载体：既可能是设备格式（WAVEFORMATEX 家族），也可能是
      // 标志位表。先试按波形格式解码，解不出就只打长度与前几个字节。
      char hex[8] = {0};
      std::string out = "len=" + std::to_string(var.blob.cbSize);
      const std::string decoded =
          FormatWaveFormat(var.blob.pBlobData, var.blob.cbSize);
      if (!decoded.empty()) return out + " " + decoded;
      const size_t n = var.blob.cbSize < kMaxBlobBytes ? var.blob.cbSize
                                                       : kMaxBlobBytes;
      out += " bytes=";
      for (size_t i = 0; i < n; ++i) {
        std::snprintf(hex, sizeof(hex), "%02x", var.blob.pBlobData[i]);
        out += hex;
        out += ' ';
      }
      return out;
    }
    default:
      char buf[32] = {0};
      std::snprintf(buf, sizeof(buf), "<VT 0x%04x 未译>", var.vt);
      return buf;
  }
}

/**
 * @brief 把 `EDataFlow` 翻成方向名。
 *
 * @param flow 数据流方向。
 * @return "Render"/"Capture"/"<其他数字>"。
 */
const char* FlowName(EDataFlow flow) {
  switch (flow) {
    case eRender:
      return "Render";
    case eCapture:
      return "Capture";
    default:
      return "<其他>";
  }
}

/**
 * @brief 把端点状态位翻成文本。
 *
 * @param state `IMMDeviceEnumerator::GetDeviceState` 取回的状态位。
 * @return ACTIVE/UNPLUGGED/NOTPRESENT 的组合，未识别的位原样附十六进制。
 * @note 判读点：状态由 0019 之类的补丁控制（"不要预载持久化的 DualSense 端点"）。
 */
std::string StateToString(DWORD state) {
  std::string out;
  if (state & DEVICE_STATE_ACTIVE) out += "ACTIVE";
  if (state & DEVICE_STATE_UNPLUGGED) out += out.empty() ? "UNPLUGGED" : "+UNPLUGGED";
  if (state & DEVICE_STATE_NOTPRESENT)
    out += out.empty() ? "NOTPRESENT" : "+NOTPRESENT";
  if (state & ~(DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED |
                DEVICE_STATE_NOTPRESENT)) {
    char buf[24] = {0};
    std::snprintf(buf, sizeof(buf), "<未知位 0x%x>",
                  static_cast<unsigned>(state));
    out += buf;
  }
  return out.empty() ? "<0>" : out;
}

/**
 * @brief 取本端点的状态，供枚举表里那一列用。
 *
 * @param device 集合里的一项。
 * @return 状态位；取不到时返回 0xFFFFFFFF（调用方据此打 "<取不到>"）。
 * @note `IMMDeviceCollection` 只给对象不给状态，状态要问 `IMMDevice::GetState`。
 *       这一列是判读点：`DEVICE_STATE_ALL` 的集合里含 NOTPRESENT（持久化下来的历史
 *       记录），那正是 DS5 补丁里"不要预载 DualSense 端点"那一类要过滤的东西。
 */
DWORD QueryState(IMMDevice* device) {
  DWORD state = 0;
  if (FAILED(device->GetState(&state))) return 0xFFFFFFFF;
  return state;
}

/**
 * @brief 取某方向某角色的默认端点 id。
 *
 * @param enumerator 已建好的设备枚举器。
 * @param flow 方向（eRender 播放 / eCapture 录音）。
 * @param role 角色（eConsole 游戏音量、eMultimedia 影音、eCommunications 通话）。
 * @return 该端点的宽字符 id；取不到返回空串。
 * @note 纯查询，不改系统状态。id 是 `CoTaskMemAlloc` 出来的字符串副本，本函数负责
 *       释放设备对象与原始串，返回的 wstring 由调用方持有。
 */
std::wstring DefaultId(IMMDeviceEnumerator* enumerator, EDataFlow flow,
                       ERole role) {
  IMMDevice* device = nullptr;
  if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, role, &device)) ||
      !device) {
    return L"";
  }
  LPWSTR id = nullptr;
  std::wstring out;
  if (SUCCEEDED(device->GetId(&id)) && id) {
    out = id;
    CoTaskMemFree(id);
  }
  device->Release();
  return out;
}

/**
 * @brief 把可能为空的宽 id 打成一行文本。
 *
 * @param id 端点 id，空串代表该方向/角色没有默认端点。
 * @return 转好的 UTF-8 串，空串换成 "<没有>"。
 */
std::string OrNone(const std::wstring& id) {
  return id.empty() ? "<没有>" : ToUtf8(id.c_str());
}

/**
 * @brief 把 `EndpointFormFactor` 枚举值翻成名字。
 *
 * @param value `PKEY_AudioEndpoint_FormFactor` 的值（VT_UI4）。
 * @return 枚举名；越界时返回 "<form factor N>"。
 * @note 这批名字是 Win32 API 的公开枚举，与我们的 `device.form-factor=controller`
 *       在本项目里没有直接对应——winepulse 不消费该键，值能出现只说明 Wine 自己
 *       填了一个默认值。
 */
std::string FormFactorName(ULONG value) {
  static constexpr const char* kNames[] = {
      "RemoteNetworkDevice", "Speakers",            "LineLevel",
      "Headphones",          "Microphone",          "Headset",
      "Handset",             "UnknownDigitalPassthrough", "SPDIF",
      "DigitalAudioDisplayDevice", "UnknownFormFactor"};
  if (value < std::size(kNames)) return kNames[value];
  return "<form factor " + std::to_string(value) + ">";
}

/**
 * @brief 打印端点属性存储的全部内容，并单独标出判读用的几项。
 *
 * @param device 要读属性的端点；本函数不持有它，也不释放。
 * @note 侧作用：写 stdout。单列出来的是：
 *       `PKEY_Device_FriendlyName`（游戏按 strstr 匹配的那份名字）、
 *       `PKEY_AudioEndpoint_FormFactor` 与 `PhysicalSpeakers`（winepulse 自填）、
 *       `PKEY_AudioEngine_DeviceFormat`（共享模式设备格式，EXTENSIBLE 时带掩码）、
 *       `PKEY_Device_ContainerId`（音频端点与 HID 设备的配对依据；Wine 侧实测为空，
 *       配对证据要另从 setupapi 的设备记录取，见 `DumpInterfaces`）。
 *       `PKEY_Device_InstanceId` 在 Wine 上同样为空，实例路径由 winepulse 合成后存在
 *       非 SDK 键 `{B3F8FA53-…}-2` 里，这一项也单独列出来。
 *       其余键一律照打，因为判据会随 GE 版本加新键。
 */
void DumpProperties(IMMDevice* device) {
  IPropertyStore* store = nullptr;
  HRESULT hr = device->OpenPropertyStore(STGM_READ, &store);
  if (FAILED(hr) || !store) {
    Print("    属性存储打不开: hr=0x" + [&] {
      char buf[12] = {0};
      std::snprintf(buf, sizeof(buf), "%08lx", static_cast<unsigned long>(hr));
      return std::string(buf);
    }());
    return;
  }

  DWORD count = 0;
  store->GetCount(&count);
  Print("    属性 " + std::to_string(count) + " 项");
  for (DWORD i = 0; i < count; ++i) {
    PROPERTYKEY key{};
    PROPVARIANT var{};
    PropVariantInit(&var);
    if (FAILED(store->GetAt(i, &key)) || FAILED(store->GetValue(key, &var))) {
      PropVariantClear(&var);
      continue;  // 单项读失败不影响其余项：探针宁可少一行也不中断整份快照。
    }
    Print("      " + PropertyKeyToString(key) + " = " + FormatPropVariant(var));
    PropVariantClear(&var);
  }

  // 这几项单独再打一遍：键名在 SDK 里散在头文件各处，判读时不该来回翻表。
  // 其中 `kWineDevicePathKey` 不是 SDK 键，但实例路径只在它那里。
  struct Interesting {
    const PROPERTYKEY* key;
    const char* name;
    bool form_factor;  // true = 值是 EndpointFormFactor 枚举，按名字打
    bool speakers;     // true = 值是 KSAUDIO_SPEAKER 掩码，按通道名打
  };
  static const Interesting kInteresting[] = {
      {&PKEY_Device_InstanceId, "实例 id（Wine 上恒为空，路径在下一行）", false,
       false},
      {&kWineDevicePathKey, "设备实例路径（Wine 的 devicepath_key）", false, false},
      {&PKEY_Device_FriendlyName, "友好名（游戏 strstr 的那份）", false, false},
      {&PKEY_Device_ContainerId, "容器 id（端点配对键）", false, false},
      {&PKEY_AudioEndpoint_FormFactor, "form factor", true, false},
      {&PKEY_AudioEndpoint_PhysicalSpeakers, "物理通道掩码", false, true},
      {&PKEY_AudioEngine_DeviceFormat, "共享模式设备格式", false, false},
  };
  for (const Interesting& item : kInteresting) {
    PROPVARIANT var{};
    PropVariantInit(&var);
    const std::string label =
        std::string("    >> ") + item.name + " " + PropertyKeyToString(*item.key) +
        " = ";
    if (FAILED(store->GetValue(*item.key, &var))) {
      Print(label + "<没有这一项>");
    } else {
      // form factor 是裸枚举值，光看数字判不出；其余按 FormatPropVariant 的通用口径。
      if (item.form_factor && var.vt == VT_UI4) {
        Print(label + FormFactorName(var.ulVal) + " (" +
              std::to_string(var.ulVal) + ")");
      } else if (item.speakers && var.vt == VT_UI4) {
        Print(label + ChannelMaskToString(var.ulVal) + " (" +
              std::to_string(var.ulVal) + ")");
      } else {
        Print(label + FormatPropVariant(var));
      }
    }
    PropVariantClear(&var);
  }
  store->Release();
}

/**
 * @brief 取该端点共享模式的混音格式，并试列几种格式的接受度。
 *
 * @param device 要激活 `IAudioClient` 的端点，播放或录音都行（IsFormatSupported 两侧都有）。
 * @note 侧作用：写 stdout；COM 对象在本函数内成对释放。`dwChannelMask` 是判读重点——
 *       上游 winepulse 用 `pulse_channel_map_to_channel_mask` 从节点的通道映射算它，
 *       GE 的 0193 再按它决定是否优先选"公开的 quad sink"。
 */
void DumpMixFormat(IMMDevice* device) {
  IAudioClient* client = nullptr;
  HRESULT hr = device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&client));
  if (FAILED(hr) || !client) {
    Print("    IAudioClient 激活失败: hr=0x" + [&] {
      char buf[12] = {0};
      std::snprintf(buf, sizeof(buf), "%08lx", static_cast<unsigned long>(hr));
      return std::string(buf);
    }());
    return;
  }

  WAVEFORMATEX* mix = nullptr;
  const HRESULT mix_hr = client->GetMixFormat(&mix);
  if (SUCCEEDED(mix_hr) && mix) {
    Print("    混音格式: " + FormatWaveFormat(
                                 reinterpret_cast<const BYTE*>(mix),
                                 sizeof(WAVEFORMATEX) + mix->cbSize) +
          " tag=0x" + [&] {
            char buf[8] = {0};
            std::snprintf(buf, sizeof(buf), "%04x",
                          static_cast<unsigned>(mix->wFormatTag));
            return std::string(buf);
          }());
  } else {
    Print("    混音格式取不到 hr=0x" + [&] {
      char buf[12] = {0};
      std::snprintf(buf, sizeof(buf), "%08lx", static_cast<unsigned long>(mix_hr));
      return std::string(buf);
    }());
  }
  if (mix) CoTaskMemFree(mix);

  // 逐个试 2ch / 4ch 的 float 与 16bit 形态：游戏（和 GE 的 split 路径）常按固定格式
  // 请求，IsFormatSupported 的答复比混音格式更能说明"这条路走不走得通"。
  struct Probe {
    WORD channels;
    WORD bits;
    const GUID* subformat;
  };
  static const Probe kProbes[] = {
      {2, 32, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT},
      {4, 32, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT},
      {2, 16, &KSDATAFORMAT_SUBTYPE_PCM},
      {4, 16, &KSDATAFORMAT_SUBTYPE_PCM}};
  for (const Probe& p : kProbes) {
    WAVEFORMATEXTENSIBLE fmt{};
    // 带通道掩码的请求只能是 EXTENSIBLE：普通 WAVEFORMATEX 没有掩码字段，
    // 而混合模式按掩码下混。采样类型住在 SubFormat 里。
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.nChannels = p.channels;
    fmt.Format.nSamplesPerSec = 48000;  // 与本项目 sink/source 的采样率同一口径
    fmt.Format.wBitsPerSample = p.bits;
    fmt.Format.nBlockAlign =
        static_cast<WORD>(p.channels * p.bits / 8);
    fmt.Format.nAvgBytesPerSec =
        48000u * fmt.Format.nBlockAlign;  // 48 kHz 下 = 每帧字节数 × 采样率
    fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    // 2ch 用立体声、4ch 用 quad：这是 Windows 侧最常见的两组默认掩码，也是判读时
    // 想看到的两种形态。
    fmt.dwChannelMask = p.channels == 2 ? KSAUDIO_SPEAKER_STEREO
                                        : KSAUDIO_SPEAKER_QUAD;
    fmt.SubFormat = *p.subformat;
    fmt.Samples.wValidBitsPerSample = p.bits;  // 有效位数 = 容器位数（float/16bit 都是满位）
    WAVEFORMATEX* closest = nullptr;
    const HRESULT probe_hr = client->IsFormatSupported(
        AUDCLNT_SHAREMODE_SHARED, &fmt.Format, &closest);
    // S_OK = 原生接受；S_FALSE = 需要转换（closest 给出要转成的格式）；其余 = 拒绝，
    // 把 HRESULT 一起打出来，判读时分得清"格式不对"和"驱动不支持"。
    std::string verdict = probe_hr == S_OK
                              ? "支持"
                              : (probe_hr == S_FALSE ? "需转换" : "拒绝 hr=0x" + [&] {
                                  char buf[12] = {0};
                                  std::snprintf(buf, sizeof(buf), "%08lx",
                                                static_cast<unsigned long>(probe_hr));
                                  return std::string(buf);
                                }());
    if (closest) {
      verdict += "（建议格式 " + std::to_string(closest->nChannels) + "ch/" +
                 std::to_string(closest->wBitsPerSample) + "bit";
      if (closest->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        verdict += " 掩码 " +
                   ChannelMaskToString(
                       reinterpret_cast<WAVEFORMATEXTENSIBLE*>(closest)
                           ->dwChannelMask);
      }
      verdict += ")";
      CoTaskMemFree(closest);
    }
    Print("      试 " + std::to_string(p.channels) + "ch/" +
          std::to_string(p.bits) + "bit @48k: " + verdict);
  }
  client->Release();
}

/**
 * @brief 枚举指定方向的全部端点，逐个打印 id、状态、属性与格式。
 *
 * @param enumerator 已建好的设备枚举器。
 * @param flow 方向（eRender 或 eCapture）。
 * @param default_console 该方向在 eConsole 角色下的默认端点 id，空串 = 没有默认。
 * @param default_multimedia 同上，eMultimedia 角色。
 * @note 侧作用：写 stdout。默认 id 由调用方在本函数之前按方向取好——本项目刻意不写
 *       `priority.session`，所以我们的 sink 不该出现在默认行上，出现了就说明有别的
 *       东西把它设成了默认设备。
 */
void DumpFlow(IMMDeviceEnumerator* enumerator, EDataFlow flow,
              const std::wstring& default_console,
              const std::wstring& default_multimedia) {
  IMMDeviceCollection* collection = nullptr;
  HRESULT hr = enumerator->EnumAudioEndpoints(
      flow, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED |
                DEVICE_STATE_NOTPRESENT,
      &collection);
  if (FAILED(hr) || !collection) {
    Print(std::string(FlowName(flow)) + " 枚举失败: hr=0x" + [&] {
      char buf[12] = {0};
      std::snprintf(buf, sizeof(buf), "%08lx", static_cast<unsigned long>(hr));
      return std::string(buf);
    }());
    return;
  }

  UINT count = 0;
  collection->GetCount(&count);
  Print(std::string("=== ") + FlowName(flow) + " 端点 " +
        std::to_string(count) + " 个 ===");

  for (UINT i = 0; i < count; ++i) {
    IMMDevice* device = nullptr;
    if (FAILED(collection->Item(i, &device)) || !device) continue;
    LPWSTR id = nullptr;
    device->GetId(&id);

    const DWORD state = QueryState(device);
    Print("[" + std::to_string(i) + "] " + ToUtf8(id));
    Print("    状态 = " +
          (state == 0xFFFFFFFF ? "<取不到>" : StateToString(state)));
    if (id && !default_console.empty() && default_console == id) {
      Print("    << eConsole 角色默认端点");
    }
    if (id && !default_multimedia.empty() && default_multimedia == id) {
      Print("    << eMultimedia 角色默认端点");
    }
    // 端点自己声明的方向与枚举方向分开取、分开打：两者不一致时只看集合会判错。
    EDataFlow reported = flow;
    bool have_reported = false;
    IMMEndpoint* endpoint = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_IMMEndpoint,
                                         reinterpret_cast<void**>(&endpoint))) &&
        endpoint) {
      have_reported = SUCCEEDED(endpoint->GetDataFlow(&reported));
      endpoint->Release();
    }
    Print("    数据流 = " + std::string(FlowName(reported)) +
          (have_reported ? "（IMMEndpoint::GetDataFlow）"
                         : "（取不到，按枚举方向）"));
    if (have_reported && reported != flow) {
      Print("    !! 声明方向与集合不一致: " + std::string(FlowName(reported)));
    }

    DumpProperties(device);
    DumpMixFormat(device);
    if (id) CoTaskMemFree(id);
    device->Release();
  }
  collection->Release();
}

/**
 * @brief 枚举 setupapi 里某一设备接口类的全部实例，打印路径、实例 id 与容器 id。
 *
 * @param class_guid 设备接口类 GUID（HID 用 `GUID_DEVINTERFACE_HID`，音频用
 *                   `KSCATEGORY_AUDIO` / `_RENDER` / `_CAPTURE`，定义在 `ksmedia.h`）。
 * @param label 输出里那一节的标题。
 * @return 找到的接口数；`SetupDiGetClassDevsW` 直接失败返回 -1。
 * @note 侧作用：写 stdout。这一节回答"音频端点和 HID 设备是不是被认成同一只设备"——
 *       同一个物理设备的两条接口记录应给出相等的 `DEVPKEY_Device_ContainerId`；取不到
 *       时打该行的 gle，全零 GUID 也照打（GUID_NULL 的语义是"不归属任何容器"）。
 *       容器 id 由 WineBus/SetupAPI 写在设备记录上，不在 MMDevice 属性存储里（那边实测
 *       为空，见 `DumpProperties`），所以配对证据只能从这一节取。原版 Wine 不造 KS
 *       音频接口，`KSCATEGORY_*` 三节因此恒为空；有内容说明跑的是带 GE 0003 那类伪造
 *       接口补丁的版本，且 `PROTON_ENABLE_MHWILDS_USB_AUDIO=1`（0103 把暴露范围收到
 *       这个开关下）。两次调用取长度是 SetupDi 系列的固定形态，第一次返回
 *       ERROR_INSUFFICIENT_BUFFER 属于正常流程，不当失败处理。
 */
int DumpInterfaces(const GUID& class_guid, const char* label) {
  HDEVINFO set = SetupDiGetClassDevsW(&class_guid, nullptr, nullptr,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) {
    const DWORD gle = GetLastError();
    Print(std::string(label) + ": SetupDiGetClassDevs 失败，跳过 (gle=0x" + [&] {
      char buf[12] = {0};
      std::snprintf(buf, sizeof(buf), "%08lx", static_cast<unsigned long>(gle));
      return std::string(buf);
    }() + ")");
    return -1;
  }

  Print(std::string("=== 设备接口 ") + label + " ===");
  int found = 0;
  for (DWORD index = 0;; ++index) {
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof(iface);
    if (!SetupDiEnumDeviceInterfaces(set, nullptr, &class_guid, index, &iface)) {
      break;  // ERROR_NO_MORE_ITEMS：正常结束。
    }

    DWORD needed = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &needed, nullptr);
    if (needed == 0) continue;
    // SP_DEVICE_INTERFACE_DETAIL_DATA_W 的 cbSize 是结构体本身的大小，不是缓冲区
    // 大小——这是 SetupDi 的定长头部约定，写错了整个调用会静默失败。
    std::vector<BYTE> buffer(needed);
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
        buffer.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    SP_DEVINFO_DATA device_info{};
    device_info.cbSize = sizeof(device_info);
    if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, needed, nullptr,
                                          &device_info)) {
      ++found;
      Print("  [接口] <Detail 取不到, needed=" + std::to_string(needed) + ">");
      continue;
    }
    ++found;
    Print("  [接口] " + ToUtf8(detail->DevicePath));

    DEVPROPTYPE type = 0;
    BYTE prop[512] = {0};
    DWORD size = sizeof(prop);
    // 实例 id 与容器 id 分别取：DEVPKEY_Device_InstanceId 是字符串、ContainerId 是
    // GUID，同一缓冲复用。
    if (SetupDiGetDevicePropertyW(set, &device_info, &DEVPKEY_Device_InstanceId,
                                  &type, prop, size, &size, 0)) {
      Print("       实例 id = " + ToUtf8(reinterpret_cast<WCHAR*>(prop)));
    } else {
      Print("       实例 id = <取不到 gle=0x" + [&] {
        char buf[12] = {0};
        std::snprintf(buf, sizeof(buf), "%08lx>",
                      static_cast<unsigned long>(GetLastError()));
        return std::string(buf);
      }());
    }
    size = sizeof(prop);
    if (SetupDiGetDevicePropertyW(set, &device_info, &DEVPKEY_Device_ContainerId,
                                  &type, prop, size, &size, 0)) {
      Print("       容器 id = " +
            GuidToString(*reinterpret_cast<GUID*>(prop)));
    } else {
      Print("       容器 id = <没有这项 gle=0x" + [&] {
        char buf[12] = {0};
        std::snprintf(buf, sizeof(buf), "%08lx>",
                      static_cast<unsigned long>(GetLastError()));
        return std::string(buf);
      }());
    }
  }
  SetupDiDestroyDeviceInfoList(set);
  return found;
}

/**
 * @brief 打印 DS5 补丁链读取的那些环境变量的当前值。
 *
 * @note 侧作用：写 stdout。这些开关决定同一份节点属性会不会被当成"手柄的喇叭"，
 *       没设的时候判读要换个分支，所以每一行都固定打出来（未设置打 `<未设置>`）。
 */
void DumpEnvGate() {
  static constexpr const char* kVars[] = {
      "PROTON_DUALSENSE_SPLIT_AUDIO",
      "PROTON_DEATH_STRANDING_CONTROLLER_EFFECTS",
      "PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE",
      "PROTON_SONY_WINDOWS_DEVICE_NAMES",
      "PROTON_DUALSENSE_HAPTICS_PREFER_NON_EVENT",
      "PROTON_ENABLE_MHWILDS_USB_AUDIO",
      "PROTON_MMDEV_FAKE_EXCLUSIVE"};
  Print("=== 补丁开关（环境变量）===");
  for (const char* name : kVars) {
    WCHAR buf[128] = {0};
    WCHAR wide[128] = {0};
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wide,
                        static_cast<int>(std::size(wide)));
    // 返回值是读到的字符数（不含终止符）；0 = 变量不存在或缓冲不够，两者在 128 字符
    // 缓冲下只有前者可能出现。
    const DWORD got = GetEnvironmentVariableW(wide, buf, static_cast<DWORD>(std::size(buf)));
    Print(std::string("  ") + name + " = " +
          (got == 0 ? "<未设置>" : "\"" + ToUtf8(buf) + "\""));
  }
}

/**
 * @brief 往指定端点灌一段正弦，只在指定通道上有能量。
 *
 * @param enumerator 设备枚举器，用来按 id 子串取端点。
 * @param needle 端点 id 里要匹配的子串（宽字符，大小写敏感的子串相等）。
 * @param channel 有信号的通道，1 起；其余通道写 0。
 * @param seconds 持续时长（秒）。
 * @return 0 = 成功灌完；1 = 没匹配到端点或激活/初始化失败；2 = 混音格式不是 float。
 * @note 侧作用：在目标端点上建立一条共享模式渲染流并推送样本，退出前 Stop+释放。
 *       这条路径用来验一个 PipeWire 侧看不出结论的问题：节点上第 3/4 通道的音量若被
 *       存成 0，游戏写进触觉通道的数据会在服务端被乘成 0，本工具灌的信号同样会消失——
 *       两相对照（本工具的输出与 worker 侧的触觉统计）就能判定是哪一层丢的。
 *       缓冲推送用 `GetCurrentPadding` 控制深度，没有自起线程。
 */
int WriteTone(IMMDeviceEnumerator* enumerator, const std::wstring& needle,
              int channel, int seconds) {
  IMMDeviceCollection* collection = nullptr;
  if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                            &collection)) ||
      !collection) {
    Print("灌信号: 找不到活动端点");
    return 1;
  }

  IMMDevice* device = nullptr;
  UINT count = 0;
  collection->GetCount(&count);
  for (UINT i = 0; i < count && !device; ++i) {
    IMMDevice* candidate = nullptr;
    if (FAILED(collection->Item(i, &candidate)) || !candidate) continue;
    LPWSTR id = nullptr;
    candidate->GetId(&id);
    // 只取第一个命中：目标是本工具自己打出来的那行完整 id，正常情况不会有两个候选。
    if (id && wcsstr(id, needle.c_str()) != nullptr) {
      device = candidate;
      Print("灌信号: 命中端点 " + ToUtf8(id));
    }
    if (id) CoTaskMemFree(id);
    if (device) break;
    candidate->Release();
  }
  collection->Release();
  if (!device) {
    Print("灌信号: 没有 active 的 eRender 端点的 id 含 \"" + ToUtf8(needle.c_str()) +
          "\"");
    return 1;
  }

  IAudioClient* client = nullptr;
  HRESULT hr = device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&client));
  device->Release();
  if (FAILED(hr) || !client) {
    Print("灌信号: IAudioClient 激活失败 hr=0x" + [&] {
      char buf[12] = {0};
      std::snprintf(buf, sizeof(buf), "%08lx", static_cast<unsigned long>(hr));
      return std::string(buf);
    }());
    return 1;
  }

  WAVEFORMATEX* mix = nullptr;
  client->GetMixFormat(&mix);
  if (!mix || !IsFloat32Format(mix)) {
    Print("灌信号: 混音格式不是 32 位 float，本工具只按 float 填样本");
    if (mix) CoTaskMemFree(mix);
    client->Release();
    return 2;
  }
  // Initialize 的缓冲时长单位是 100 ns：0.1 s = 1e6 个单位。共享模式下这个值只是
  // 调用方期望的深度，实际缓冲大小由 `GetBufferSize` 事后问出来。
  constexpr REFERENCE_TIME kBufferDuration100ns = 1'000'000;
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBufferDuration100ns, 0,
                          mix, nullptr);
  if (FAILED(hr)) {
    Print("灌信号: Initialize 失败 hr=0x" + [&] {
      char buf[12] = {0};
      std::snprintf(buf, sizeof(buf), "%08lx", static_cast<unsigned long>(hr));
      return std::string(buf);
    }());
    CoTaskMemFree(mix);
    client->Release();
    return 1;
  }

  UINT buffer_frames = 0;
  client->GetBufferSize(&buffer_frames);
  IAudioRenderClient* render = nullptr;
  hr = client->GetService(IID_IAudioRenderClient,
                          reinterpret_cast<void**>(&render));
  if (FAILED(hr) || !render) {
    Print("灌信号: IAudioRenderClient 取不到");
    CoTaskMemFree(mix);
    client->Release();
    return 1;
  }
  // 不带 AUDCLNT_STREAMFLAGS_EVENTCALLING 的共享模式必须显式 Start，否则设备引擎不
  // 消费缓冲，`GetCurrentPadding` 会一路涨到写满后卡死。
  if (FAILED(client->Start())) {
    Print("灌信号: Start 失败，中止");
    render->Release();
    CoTaskMemFree(mix);
    client->Release();
    return 1;
  }

  Print("灌信号: 通道 " + std::to_string(channel) + "/" +
        std::to_string(mix->nChannels) + "，时长 " + std::to_string(seconds) +
        " s，缓冲 " + std::to_string(buffer_frames) + " 帧 @" +
        std::to_string(mix->nSamplesPerSec) + "Hz");

  // 相位按已写帧数累加，不用墙钟：这样跨段边界不会跳相，整段是同一个连续正弦。
  unsigned long long written = 0;
  const unsigned long long total =
      static_cast<unsigned long long>(mix->nSamplesPerSec) *
      static_cast<unsigned long long>(seconds);
  const WORD channels = mix->nChannels;
  int errors = 0;
  while (written < total) {
    UINT padding = 0;
    if (FAILED(client->GetCurrentPadding(&padding))) break;
    // 把引擎已经腾出来的空间一次写完：容量 = 缓冲总帧数 - 还在排的帧数。
    const UINT capacity = buffer_frames > padding ? buffer_frames - padding : 0;
    if (capacity == 0) {
      Sleep(2);  // 设备没腾出空间，等一小段再看。
      continue;
    }
    const unsigned long long remaining = total - written;
    const UINT to_write = remaining < capacity ? static_cast<UINT>(remaining)
                                               : capacity;
    BYTE* data = nullptr;
    hr = render->GetBuffer(to_write, &data);
    if (FAILED(hr) || !data) {
      // 累计失败到一定次数就收摊，不让这段循环变成忙等（Wine 侧共享流实现不完整时
      // 就是这种表现）。
      if (++errors > 20) break;
      continue;
    }
    errors = 0;
    auto* frames = reinterpret_cast<float*>(data);
    for (UINT f = 0; f < to_write; ++f) {
      const double phase = 2.0 * 3.141592653589793 *
                           static_cast<double>(written + f) * kToneFrequencyHz /
                           static_cast<double>(mix->nSamplesPerSec);
      // 0.7 ≈ -3 dBFS：留出余量，避免下游音量合成一抬就削顶。
      const float sample = static_cast<float>(std::sin(phase)) * 0.7f;
      for (WORD c = 0; c < channels; ++c) {
        // 通道号按用户口径 1 起，数组下标 0 起；只有一路有能量，其余写 0。
        frames[static_cast<size_t>(f) * channels + c] =
            (static_cast<int>(c) + 1 == channel) ? sample : 0.0f;
      }
    }
    render->ReleaseBuffer(to_write, 0);
    written += to_write;
    if (written >= total) break;
    Sleep(kWriteTickMs);  // 让出 STA 线程，和 event-driven 客户端的节奏一致。
  }
  client->Stop();
  render->Release();
  CoTaskMemFree(mix);
  client->Release();
  Print("灌信号: 已写 " + std::to_string(written) + " 帧");
  return 0;
}

}  // namespace

/**
 * @brief 探针入口：解析开关、按方向枚举、读设备接口、可选灌信号。
 *
 * @param argc 参数个数。
 * @param argv 参数数组（宽字符，用 `-municode` 时 CRT 直接给 UTF-16）。
 * @return 0 = 跑完；1 = COM 初始化失败（后面的枚举没有意义，直接退）。
 * @note 侧作用：写 stdout。`--write` 只在 eRender 上取端点；其余输出恒为只读快照，
 *       不改变任何系统状态。
 */
int wmain(int argc, wchar_t** argv) {
  std::wstring target;      // --write 的 id 子串，空 = 不灌信号
  int channel = kDefaultWriteChannel;
  int seconds = kDefaultWriteSeconds;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg(argv[i]);
    if (arg == L"--write") {
      // --write 后面跟目标端点 id 的子串，直接抄上面打出来的那行即可。
      if (i + 1 < argc) target = argv[++i];
    } else if (arg == L"--ch" && i + 1 < argc) {
      channel = _wtoi(argv[++i]);
    } else if (arg == L"--secs" && i + 1 < argc) {
      seconds = _wtoi(argv[++i]);
    }
  }

  const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(co)) {
    Print("CoInitializeEx 失败 hr=0x" + [&] {
      char buf[12] = {0};
      std::snprintf(buf, sizeof(buf), "%08lx", static_cast<unsigned long>(co));
      return std::string(buf);
    }());
    return 1;
  }

  Print(std::string("win_audio_probe — Windows 侧音频环境快照"));
  DumpEnvGate();

  IMMDeviceEnumerator* enumerator = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                IID_IMMDeviceEnumerator,
                                reinterpret_cast<void**>(&enumerator));
  if (FAILED(hr) || !enumerator) {
    Print("MMDeviceEnumerator 建不出来 hr=0x" + [&] {
      char buf[12] = {0};
      std::snprintf(buf, sizeof(buf), "%08lx", static_cast<unsigned long>(hr));
      return std::string(buf);
    }());
    CoUninitialize();
    return 1;
  }

  // 两个角色的默认端点先按方向取出来，既单独打一行，也交给 DumpFlow 做标注：本项目
  // 刻意不填 priority.session，所以这条 sink 不该出现在默认行上，出现了就说明有别的
  // 东西把它设成了默认。
  const std::wstring console_render = DefaultId(enumerator, eRender, eConsole);
  const std::wstring mm_render = DefaultId(enumerator, eRender, eMultimedia);
  const std::wstring console_capture = DefaultId(enumerator, eCapture, eConsole);
  const std::wstring mm_capture = DefaultId(enumerator, eCapture, eMultimedia);
  Print("=== 默认端点（渲染）===");
  Print("  eConsole    = " + OrNone(console_render));
  Print("  eMultimedia = " + OrNone(mm_render));
  Print("=== 默认端点（采集）===");
  Print("  eConsole    = " + OrNone(console_capture));
  Print("  eMultimedia = " + OrNone(mm_capture));

  DumpFlow(enumerator, eRender, console_render, mm_render);
  DumpFlow(enumerator, eCapture, console_capture, mm_capture);

  DumpInterfaces(GUID_DEVINTERFACE_HID, "HID（GUID_DEVINTERFACE_HID）");
  DumpInterfaces(KSCATEGORY_AUDIO, "音频类别（KSCATEGORY_AUDIO）");
  DumpInterfaces(KSCATEGORY_RENDER, "渲染类别（KSCATEGORY_RENDER）");
  DumpInterfaces(KSCATEGORY_CAPTURE, "采集类别（KSCATEGORY_CAPTURE）");

  if (!target.empty()) {
    Print("=== 灌信号 ===");
    WriteTone(enumerator, target, channel, seconds);
  }

  enumerator->Release();
  CoUninitialize();
  Print("=== 结束 ===");
  return 0;
}
