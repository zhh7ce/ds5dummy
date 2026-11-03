/**
 * @file daemon/discovery.cpp
 * @brief sysfs 扫描实现：按 HID 总线 + VID/PID + HID_UNIQ 认出 DualSense
 * @details 每个节点只读 device/uevent 一行的三类信息：HID_ID 给出总线与
 *          VID/PID，HID_UNIQ 给出 BD_ADDR。二者缺一都认不出手柄身份，
 *          其余字段（HID_NAME、HID_PHYS）与本程序无关。扫描本身不写日志：
 *          Manager 会按"新出现 / 已服务"决定说什么，这里保持纯查询。
 */

#include "daemon/discovery.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/device/identity.h"
#include "daemon/config.h"

namespace fs = std::filesystem;

namespace ds5::daemon {
namespace {

/**
 * @brief 一个 hidraw 节点 uevent 里本模块关心的字段。
 * @note 全为 0/空的默认值表示"没读到"，由 ReadNodeIdentity 的返回值区分。
 */
struct NodeIdentity {
  uint32_t bus = 0;  ///< HID_ID 的总线段，0x0005 = Bluetooth HID。
  uint32_t vid = 0;  ///< HID_ID 的 VID 段。
  uint32_t pid = 0;  ///< HID_ID 的 PID 段。
  std::string uniq;  ///< HID_UNIQ 原文；节点没有该行时为空。
};

/**
 * @brief 读一个 hidraw 节点的 device/uevent，抽出总线号、VID/PID 与 UNIQ。
 * @param hidraw_dir sysfs 里的单个 hidraw 节点目录（如 /sys/class/hidraw/hidraw3）。
 * @param out 成功时写入读到的字段；未出现的行保持默认值。
 * @return 读到可解析的 HID_ID 行 true；文件打不开或没有该行 false。
 * @note 单行缓冲 512 字节：uevent 的键值行远短于此；超长行被 fgets 切开后
 *       前缀落在段首之外，最坏结果是这一只认不出来，不会读越界。
 */
bool ReadNodeIdentity(const fs::path& hidraw_dir, NodeIdentity& out) {
  // "re"：只读 + 关本进程泄漏出去的 fd。
  FILE* f = fopen((hidraw_dir / "device" / "uevent").c_str(), "re");
  if (!f) return false;
  bool saw_id = false;
  char line[512];
  while (fgets(line, sizeof line, f)) {
    if (strncmp(line, "HID_ID=", 7) == 0) {
      // 形如 "HID_ID=0005:0000054C:00000CE6"，三段都是十六进制。
      unsigned bus = 0;
      unsigned vid = 0;
      unsigned pid = 0;
      if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &vid, &pid) == 3) {
        out.bus = bus;
        out.vid = vid;
        out.pid = pid;
        saw_id = true;
      }
    } else if (strncmp(line, "HID_UNIQ=", 9) == 0) {
      out.uniq = line + 9;
      while (!out.uniq.empty() &&
             (out.uniq.back() == '\n' || out.uniq.back() == '\r')) {
        out.uniq.pop_back();  // 去掉行尾换行，MAC 才是 17 个字符
      }
    }
  }
  fclose(f);
  return saw_id;
}

/**
 * @brief 节点是否是我们认的手柄：蓝牙总线上，真机或合成 VID/PID。
 * @param id 已读到的节点身份。
 * @return 命中 true。
 * @details 总线号过滤掉 USB 直连的那只（0x0003），它不归本程序桥接。
 *          手柄型号的判断（DualSense / Edge、真机 / 合成）收在
 *          core::device::IsDs5HidIdentity，与 hidraw 后端共用同一张名单。
 */
bool IsOurController(const NodeIdentity& id) {
  return id.bus == 0x0005 &&
         core::device::IsDs5HidIdentity(id.vid, id.pid);
}

}  // namespace

std::vector<DiscoveredController> ScanControllers(const std::string& sysfs_base) {
  // 用 map 收集：MAC 是天然的去重键，顺便让输出按 MAC 升序，调用方与测试
  // 都不依赖目录遍历顺序。
  std::map<std::string, std::string> by_mac;

  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(sysfs_base, ec)) {
    NodeIdentity id;
    if (!ReadNodeIdentity(entry.path(), id)) continue;
    if (!IsOurController(id)) continue;
    // 没有合法 MAC 就没有分发依据（auto 模式下还得靠它建 worker 与 gadget
    // 实例名），跳过而不是拿空串去起站。
    if (!IsValidBtAddr(id.uniq)) continue;

    const std::string mac = CanonicalBtAddr(id.uniq);
    const std::string node = entry.path().filename().string();
    // 同一只手柄理论上只有一个 HID 节点；真出现两个时保留节点名最小的，
    // 让每轮扫描对同一份 sysfs 给出同一个答案。
    const auto [it, inserted] = by_mac.emplace(mac, node);
    if (!inserted && node < it->second) it->second = node;
  }

  std::vector<DiscoveredController> result;
  result.reserve(by_mac.size());
  for (auto& [mac, node] : by_mac) {
    result.push_back(DiscoveredController{mac, std::move(node)});
  }
  return result;
}

}  // namespace ds5::daemon
