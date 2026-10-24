/**
 * @file gadget_setup.cpp
 * @brief configfs 装配实现：目录树、属性写入、mount 与 UDC 绑解
 *
 * configfs 的交互纪律决定了本文件的 helper 形态：属性是内核虚拟文件，
 * 值被拒只在 write(2) 的 errno 上报（缓冲写会把错误拖到 close 之后，难以
 * 定位）；group 的删除只能是 rmdir，属性文件不可 unlink。gadget 目录名与
 * ffs function 实例名由调用方每次传入（同一进程可以持有多个 gadget），
 * FunctionFS 挂载点同样现传，所以路径全部现算，没有跨调用的状态。
 *
 * bind 之后本文件还负责一件事：从 configfs 的 UDC 属性反查主机侧枚举出来的
 * 设备目录。那只是读 sysfs，不改任何内核状态，但路径换算和虚拟文件系统
 * 打交道的方式与 configfs 同源，所以放在同一层。
 */

#include "usb/gadget_setup.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <system_error>

#include "core/debug.h"
#include "core/device/descriptors.h"
#include "loguru.hpp"
#include "usb/usb.h"

namespace fs = std::filesystem;

namespace usb {
namespace detail {

// 匿名 namespace 收目录派生与 configfs 读写 helper；对外公共面即
// gadget_setup.h 声明的函数。
namespace {

/// configfs 里一个 gadget 的根目录：所有属性与 group 路径都从它派生。
fs::path GadgetDir(const std::string& instance) {
  return fs::path(kConfigfsBase) / instance;
}

/// configfs 规定的 ffs function 名字：`ffs.<实例名>`，functions 下的目录名与
/// configs/c.1 下的符号链接名都用它。
std::string FfsLinkName(const std::string& instance) {
  return "ffs." + instance;
}

/// ffs function 的实例目录：<gadget>/functions/ffs.<name>，符号链接指向它。
fs::path FfsFuncDir(const std::string& instance) {
  return GadgetDir(instance) / "functions" / FfsLinkName(instance);
}

/// configuration c.1 目录：function 挂进它才算进 configuration。
fs::path ConfigDir(const std::string& instance) {
  return GadgetDir(instance) / "configs" / "c.1";
}

/**
 * @brief 用低层 open/write/close 写一个 configfs 属性并把 errno 带回调用方。
 *
 * 刻意绕开 ofstream/stdio：被拒的值只在 write(2) 上报，缓冲写会把错误
 * 拖到 close 之后甚至吞掉。
 *
 * @param path 属性文件路径。
 * @param value 要写入的原始字节串（可含换行，语义由内核属性决定）。
 * @param err 出参：write 失败时的 errno；成功时为 0，open 失败时也填。
 * @return 写入成功 true。
 */
bool write_attr_raw(const fs::path& path, const std::string& value, int& err) {
  err = 0;
  int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    err = errno;
    DS5DUMMY_LOG(ERROR, "[gadget] Cannot open {}: {}", path.c_str(), strerror(err));
    return false;
  }
  ssize_t n = ::write(fd, value.data(), value.size());
  err = errno;
  ::close(fd);
  if (n < 0) {
    DS5DUMMY_LOG(ERROR, "[gadget] Write to {} failed: {}", path.c_str(),
          strerror(err));
    return false;
  }
  return true;
}

/**
 * @brief write_attr_raw 的免 err 版本：调用方只关心成败。
 * @param path 属性文件路径。
 * @param value 属性值。
 * @return 写入成功 true。
 */
bool write_attr(const fs::path& path, const std::string& value) {
  int err = 0;
  return write_attr_raw(path, value, err);
}

/**
 * @brief rmdir 一个 configfs group；不存在视为已清理。
 *
 * group 只能整体 rmdir：属性文件是虚拟的、不可 unlink，任何递归删除都会
 * 在属性上撞 EPERM。
 *
 * @param path group 路径。
 * @note 失败只记日志不中断：清理路径尽力删，删不净由下一次 Setup 的
 *       "存在即先拆"兜底。
 */
void remove_configfs(const fs::path& path) {
  std::error_code ec;
  fs::remove(path, ec);
  if (ec && ec != std::errc::no_such_file_or_directory) {
    DS5DUMMY_LOG(ERROR, "[gadget] Could not remove {}: {}", path.c_str(),
          ec.message().c_str());
  }
}

/**
 * @brief 读一个 configfs 属性的首行文本并去掉行尾换行符。
 * @param path 属性文件路径。
 * @return 属性值；打不开文件返回空串（与"属性本就是空"不可区分，调用方
 *        按空串语义处理）。
 * @note 属性是内核里的短文本，64 字节缓冲够用；stdio 读没有写侧的
 *       errno 时序问题。
 */
std::string read_attr(const fs::path& path) {
  FILE* f = fopen(path.c_str(), "re");
  if (!f) {
    return "";
  }
  char buf[64] = {};
  if (fgets(buf, sizeof(buf), f)) {
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
  }
  fclose(f);
  return buf;
}

/**
 * @brief 创建一个 configfs 目录（mkdir 语义，即建 group/属性目录）。
 * @param path 目标目录。
 * @return 成功或已存在 true；创建失败 false 并记日志。
 */
bool make_configfs_dir(const fs::path& path) {
  std::error_code ec;
  fs::create_directories(path, ec);
  if (ec) {
    DS5DUMMY_LOG(ERROR, "[gadget] ERROR: cannot create {}: {}", path.c_str(),
          ec.message().c_str());
    return false;
  }
  return true;
}

/**
 * @brief 在 configfs 里建符号链接——这是把 function 挂进 configuration 的
 *        规定方式（写属性不行）。
 * @param target 被链接的 function group。
 * @param link 链接名（configs/c.1/ffs.<instance>）。
 * @return 成功 true。
 */
bool link_configfs_group(const fs::path& target, const fs::path& link) {
  std::error_code ec;
  fs::create_symlink(target, link, ec);
  if (ec) {
    DS5DUMMY_LOG(ERROR, "[gadget] ERROR: cannot link {}: {}", link.c_str(),
          ec.message().c_str());
    return false;
  }
  return true;
}

/**
 * @brief 挑一个还没被任何 gadget 绑走的 dummy_udc.N。
 * @return UDC 名；没有空闲项时返回空串。
 * @details "占用"以 configfs 为准：每个活跃 gadget 的 UDC 属性就是它当前
 *          绑走的控制器名。这样 N 只 gadget 各占一个虚拟控制器，互不抢。
 * @note 目录不存在（configfs 未挂 / dummy_hcd 未加载）时按"无空闲"处理。
 */
std::string FindFreeDummyUdc() {
  std::error_code ec;
  std::set<std::string> claimed;
  // configfs 未挂载时这两个循环都立即结束，claimed 保持空，下面按"无空闲"返回。
  // 先收集所有已绑定的 UDC 名（不只是本程序的 gadget），再把第一个没被占的
  // dummy_udc.N 交出去：dummy_hcd 的每个 N 只能绑一个 gadget。
  for (const auto& entry : fs::directory_iterator(kConfigfsBase, ec)) {
    const std::string bound = read_attr(entry.path() / "UDC");
    if (!bound.empty()) claimed.insert(bound);
  }
  for (const auto& entry : fs::directory_iterator("/sys/class/udc", ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("dummy_udc", 0) == 0 && claimed.count(name) == 0) {
      return name;
    }
  }
  return "";
}

/**
 * @brief 把 dummy 虚拟 UDC 的平台设备名换成与它配对的主机控制器平台设备名。
 *
 * dummy_hcd 模块加载时以同一个索引各注册一个平台设备（`dummy_hcd.N` 是主机
 * 侧的 root hub 所在，`dummy_udc.N` 是设备侧的控制器所在），两者是
 * `/sys/devices/platform/` 下的兄弟目录，不是父子。root hub 因此不在 UDC
 * 目录的上溯路径里，必须靠这个同索引的名字换算才能走到。
 *
 * @param udc_dev_name UDC 所在平台设备的目录名，形如 "dummy_udc.0"。
 * @return 配对的主机控制器目录名（"dummy_hcd.0"）；名字不以 "dummy_udc" 开头
 *         时返回空串，调用方按"找不到设备目录"处理。
 * @note 返回值只由入参的子串替换得来，不引入 '/' 或 ".."，所以拼进路径后
 *       仍受调用方那遍 IsSafeGadgetPath 约束。
 */
std::string HcdDirName(std::string_view udc_dev_name) {
  constexpr std::string_view kUdcPrefix = "dummy_udc";
  if (udc_dev_name.rfind(kUdcPrefix, 0) != 0) return "";
  return std::string("dummy_hcd") +
         std::string(udc_dev_name.substr(kUdcPrefix.size()));
}

}  // namespace

std::string FindDummyUdc() {
  // dummy_hcd 模块加载后 /sys/class/udc 才会出现 dummy_udc.N 条目。
  std::string udc_dir = "/sys/class/udc";
  if (!fs::exists(udc_dir)) {
    return "";
  }
  for (const auto& entry : fs::directory_iterator(udc_dir)) {
    std::string name = entry.path().filename().string();
    if (name.find("dummy_udc") == 0) {
      return name;
    }
  }
  return "";
}

bool GadgetSetup(const std::string& ffs_path, const std::string& instance,
                 const std::string& serial) {
  // 这两个字符串直接进 mkdir/mount/symlink，绝不能来自任何外部输入；生产路径上
  // 实例名由 MAC 派生、挂载点来自系统配置（经门面传入），这里按白名单再挡一遍。
  if (!IsSafeGadgetPath(ffs_path) || !IsSafeGadgetPath(instance) ||
      instance.empty()) {
    DS5DUMMY_LOG(ERROR, "[gadget] ERROR: rejected unsafe path: {} / {}",
          ffs_path.c_str(), instance.c_str());
    return false;
  }

  const fs::path gadget_dir = GadgetDir(instance);

  // usb_gadget 目录只在 configfs 挂载后出现；EBUSY=已挂载，直接续用。
  if (mount("none", "/sys/kernel/config", "configfs", 0, nullptr) != 0 &&
      errno != EBUSY) {
    DS5DUMMY_LOG(ERROR, "[gadget] ERROR: mount configfs: {}", strerror(errno));
    return false;
  }
  // 有的发行版把 configfs 默认挂成只读。调用方是 root，在这里 remount
  // 成可写，省掉对 systemd ExecStartPre 挂载命令的外部依赖。
  if (::access("/sys/kernel/config", W_OK) != 0) {
    if (mount(nullptr, "/sys/kernel/config", nullptr, MS_REMOUNT, nullptr) !=
        0) {
      DS5DUMMY_LOG(ERROR, "[gadget] ERROR: remount configfs rw: {}", strerror(errno));
      return false;
    }
  }

  // 上次崩溃的残留会让 mkdir 冲突：存在即先完整拆一遍。只拆本实例名的
  // 那一站——别的 gadget（另一只手柄）是活跃的，不能碰。
  if (fs::exists(gadget_dir)) {
    DS5DUMMY_LOG(INFO, "[gadget] Existing gadget {} found, cleaning up...",
          instance.c_str());
    GadgetTeardown(ffs_path, instance);
  }

  DS5DUMMY_LOG(INFO, "[gadget] Creating gadget: {}", instance.c_str());

  if (!make_configfs_dir(gadget_dir)) {
    return false;
  }

  // 设备描述符属性：idVendor/idProduct 被拒是致命的——那会以内核默认身份
  // 枚举上总线，而不是 DualSense。数值与 core::device 的常量对应。
  if (!write_attr(gadget_dir / "bcdUSB", "0x0200") ||
      !write_attr(gadget_dir / "idVendor", "0x054c") ||
      !write_attr(gadget_dir / "idProduct", "0x0ce6") ||
      !write_attr(gadget_dir / "bcdDevice", "0x0100") ||
      !write_attr(gadget_dir / "bMaxPacketSize0", "64")) {
    return false;
  }

  // 语言 0x409（en-US）下的三个设备字符串。序列号由调用方按站传入：主机侧把
  // iSerialNumber 当设备实例标识，多站同值会被认成同一台设备反复插拔。
  fs::path strings_dir = gadget_dir / "strings" / "0x409";
  if (!make_configfs_dir(strings_dir)) {
    return false;
  }
  if (!write_attr(strings_dir / "manufacturer", core::device::kManufacturer) ||
      !write_attr(strings_dir / "product", core::device::kProduct) ||
      !write_attr(strings_dir / "serialnumber", serial)) {
    return false;
  }

  // FunctionFS function 实例目录。
  const fs::path ffs_func_dir = FfsFuncDir(instance);
  if (!make_configfs_dir(ffs_func_dir)) {
    return false;
  }

  // functionfs 只能挂在已存在的 function 实例名上：名不匹配时 mount 以
  // ENOENT 失败，因此必须先建目录再挂。
  if (!make_configfs_dir(ffs_path)) {
    return false;
  }
  if (mount(instance.c_str(), ffs_path.c_str(), "functionfs", 0, nullptr) != 0) {
    DS5DUMMY_LOG(ERROR, "[gadget] ERROR: mount functionfs on {}: {}", ffs_path.c_str(),
          strerror(errno));
    return false;
  }
  DS5DUMMY_LOG(INFO, "[gadget] Mounted FunctionFS on {}", ffs_path.c_str());

  // configuration c.1 与其字符串。
  const fs::path config_dir = ConfigDir(instance);
  fs::path config_strings_dir = config_dir / "strings" / "0x409";
  if (!make_configfs_dir(config_strings_dir)) {
    return false;
  }
  // 这两条是外观属性：写失败不影响总线功能，故意不检查返回值。
  write_attr(config_dir / "MaxPower", "500");
  write_attr(config_strings_dir / "configuration", "DS5 Dummy Config");

  // 符号链接把 ffs function 挂进 configuration（configfs 的规定接法）。
  if (!link_configfs_group(ffs_func_dir, config_dir / FfsLinkName(instance))) {
    return false;
  }

  DS5DUMMY_LOG(INFO, "[gadget] USB gadget configured (not yet bound)");

  return true;
}

bool GadgetBind(const std::string& instance) {
  std::string udc = FindFreeDummyUdc();
  if (udc.empty()) {
    DS5DUMMY_LOG(ERROR,
          "[gadget] ERROR: No free dummy_hcd UDC. Load/expand dummy_hcd: "
          "sudo modprobe dummy_hcd is_high_speed=0 num_dummy=<controllers>");
    return false;
  }

  const fs::path gadget_dir = GadgetDir(instance);
  if (!write_attr(gadget_dir / "UDC", udc)) {
    // composite bind 把所有被拒原因折叠成一个 errno，具体是哪个 function
    // 反对只存在于内核日志——把两条线索都打给运维。
    std::string ready = read_attr(FfsFuncDir(instance) / "ready");
    DS5DUMMY_LOG(ERROR,
          "[gadget] ERROR: Could not bind {} to UDC {} "
          "(FunctionFS ready={}).",
          instance.c_str(), udc.c_str(), ready.c_str());
    DS5DUMMY_LOG(ERROR,
          "[gadget] Run 'journalctl -k | tail' for the failing function.");
    return false;
  }

  DS5DUMMY_LOG(INFO, "[gadget] {} bound to UDC: {}", instance.c_str(),
        udc.c_str());
  return true;
}

std::string FindHostDeviceDir(const std::string& udc_sysfs_path,
                              uint16_t vendor_id, uint16_t product_id) {
  // 入参来自 canonical 结果，仍然挡一遍：它进的是目录遍历，字符集和 ".."
  // 的约束与建路径时一致。
  if (!IsSafeGadgetPath(udc_sysfs_path)) return "";

  // canonical 结果形如 /sys/devices/platform/dummy_udc.N/udc/dummy_udc.N：
  // 上溯两级是 UDC 自己的平台设备，root hub 不在它下面，而在同索引的兄弟目录
  // dummy_hcd.N 里（见 HcdDirName）。
  const fs::path udc_dev =
      fs::path(udc_sysfs_path).parent_path().parent_path();
  const std::string hcd_name = HcdDirName(udc_dev.filename().string());
  if (hcd_name.empty()) return "";  // 不是 dummy_udc.N：无从配对。
  const fs::path hcd_dir = udc_dev.parent_path() / hcd_name;

  // 期望值按 sysfs 的口径格式化：idVendor/idProduct 由内核以 %04x 输出，
  // 所以是小写 4 位十六进制、无 0x 前缀。
  const std::string want_vendor = std::format("{:04x}", vendor_id);
  const std::string want_product = std::format("{:04x}", product_id);

  // 控制器目录下的同级条目里以 `usb` 开头的只有 root hub（driver/subsystem/
  // power 这些平台设备通用条目都不是）。
  std::error_code ec;
  for (const auto& hub : fs::directory_iterator(hcd_dir, ec)) {
    const std::string hub_name = hub.path().filename().string();
    if (hub_name.rfind("usb", 0) != 0) continue;
    // root hub 的直接孩子才是挂在它上面的设备；hub 自身也有 idVendor/idProduct
    // （内核 HCD 的 1d6b），所以只在孩子这一层比。
    for (const auto& dev : fs::directory_iterator(hub.path(), ec)) {
      if (read_attr(dev.path() / "idVendor") == want_vendor &&
          read_attr(dev.path() / "idProduct") == want_product) {
        return dev.path().string();
      }
    }
  }
  // 走到这里说明设备目录还没建出来（内核 usb 核的枚举是异步的）或身份不符，
  // 两者对调用方都是"稍后再试"。
  return "";
}

std::string FindHostInterfaceDir(const std::string& device_dir) {
  // 与建路径时同一套字符白名单：入参来自 sysfs 遍历的结果，仍然挡一遍 ".."。
  if (!IsSafeGadgetPath(device_dir)) return "";

  // sysfs 把接口目录命名成 `<设备名>:<配置号>.<接口号>`（`9-1:1.0`），所以前缀
  // 就是设备目录名加一个冒号；同一层里其余条目（`usb`、`driver`、`subsystem`、
  // `power`、属性文件）都不带冒号，不会误命中。
  const std::string want_prefix =
      fs::path(device_dir).filename().string() + ":";

  std::error_code ec;
  std::string smallest;
  for (const auto& entry : fs::directory_iterator(device_dir, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind(want_prefix, 0) != 0) continue;
    // 字典序最小：取哪一个算出的父设备都一样，这里只要结果跨重启稳定。
    if (smallest.empty() || name < smallest) smallest = name;
  }
  // 目录不存在或读不动（枚举还在进行中）时 directory_iterator 带着 ec 空转完，
  // 与"一个接口都没有"落到同一个空串返回，对调用方都是"稍后再试"。
  if (smallest.empty()) return "";

  return (fs::path(device_dir) / smallest).string();
}

std::string FindHostDevicePath(const std::string& instance, uint16_t vendor_id,
                               uint16_t product_id) {
  // UDC 属性是 GadgetBind 写进去的控制器名，本站独有——用它而不是遍历
  // /sys/bus/usb/devices，避免把另一台同身份设备（例如用户真插了一只有线
  // DualSense）认成自己。
  const std::string udc = read_attr(GadgetDir(instance) / "UDC");
  if (udc.empty() || !IsSafeGadgetPath(udc)) return "";

  std::error_code ec;
  const fs::path udc_dir = fs::canonical(fs::path("/sys/class/udc") / udc, ec);
  if (ec) return "";  // 控制器已从 sysfs 消失：解绑了或模块被卸载。

  const std::string device_dir =
      FindHostDeviceDir(udc_dir.string(), vendor_id, product_id);
  if (device_dir.empty()) return "";

  // 交出的是设备**下面**的接口目录，不是设备目录自己：读这个键的那处代码
  // （Wine 的 `get_container_id`）从本目录沿父链找 `usb`/`usb_device` 祖先，
  // 再取那个祖先的 PRODUCT/BUSNUM/DEVNUM 算容器标识。设备目录本身就是那个祖先，
  // 父链会把它跳过、命中 root hub（身份是内核 HCD 的），算出来的容器标识就不属于
  // 本站，音频端点和 HID 端点配不上对。
  const std::string leaf_dir = FindHostInterfaceDir(device_dir);
  if (leaf_dir.empty()) return "";  // 接口目录还没建出来：同一套"稍后再试"。

  // device.sysfs.path 的口径是相对 /sys：切掉前缀但保留开头的 '/'，
  // 与真声卡上读到的 `/devices/pci…/sound/card1` 形态一致。
  constexpr std::string_view kSysPrefix = "/sys/";
  if (leaf_dir.rfind(kSysPrefix, 0) != 0) return leaf_dir;
  return "/" + leaf_dir.substr(kSysPrefix.size());
}

bool GadgetUnbind(const std::string& instance) {
  const fs::path gadget_dir = GadgetDir(instance);
  fs::path udc_attr = gadget_dir / "UDC";
  if (!fs::exists(gadget_dir)) return true;  // gadget 本就不在：幂等成功。

  // f_fs 在最后一个端点 fd 关闭时会自动解绑（实测行为），此时 UDC 读回
  // 来是空串。先问一句，免得把"本来就没绑"报成一次写失败。
  if (read_attr(udc_attr).empty()) {
    DS5DUMMY_LOG(INFO, "[gadget] {} UDC already empty — nothing to unbind",
          instance.c_str());
    return true;
  }

  // 解绑值是单个换行——等价于 shell 的 echo '' > UDC；属性只有收到这个
  // 字节后才会在读回时显示为空。
  int err = 0;
  if (write_attr_raw(udc_attr, "\n", err)) {
    DS5DUMMY_LOG(INFO, "[gadget] {} unbound from UDC", instance.c_str());
    return true;
  }
  if (err == ENODEV) {
    // 竞态窗口：读完 UDC 之后、写之前 f_fs 自己解绑了。同样算成功。
    DS5DUMMY_LOG(INFO, "[gadget] {} already unbound (ENODEV)",
          instance.c_str());
    return true;
  }
  return false;
}

void GadgetPurgeConfigfs(const std::string& ffs_path,
                         const std::string& instance) {
  const fs::path gadget_dir = GadgetDir(instance);

  // 先 detach 上次运行残留的 functionfs 挂载，再拆它挂靠的实例目录；
  // 没挂时 umount2 失败即无操作。
  umount2(ffs_path.c_str(), MNT_DETACH);

  if (!fs::exists(gadget_dir)) {
    return;
  }

  DS5DUMMY_LOG(INFO, "[gadget] Tearing down gadget {}", instance.c_str());

  const fs::path config_dir = ConfigDir(instance);

  // rmdir 必须自底向上、先断链接后删组：每条都只对 configfs group 生效，
  // 属性文件不可 unlink（见 remove_configfs）。
  remove_configfs(config_dir / FfsLinkName(instance));
  remove_configfs(config_dir / "strings" / "0x409");
  remove_configfs(config_dir);
  remove_configfs(FfsFuncDir(instance));
  remove_configfs(gadget_dir / "strings" / "0x409");
  remove_configfs(gadget_dir);

  DS5DUMMY_LOG(INFO, "[gadget] Gadget torn down");
}

void GadgetTeardown(const std::string& ffs_path,
                    const std::string& instance) {
  if (!GadgetUnbind(instance)) {
    DS5DUMMY_LOG(WARNING, "[gadget] Unbind failed; configfs entries may stay busy");
  }
  GadgetPurgeConfigfs(ffs_path, instance);
}

}  // namespace detail

bool IsSafeGadgetPath(const std::string& path) {
  if (path.empty()) return false;
  // 白名单字符集 + 显式禁 ".."：允许 '/' 与 '.' 是为了 /dev/ffs-ds5 这类
  // 路径与 0x409 式名字，但目录上跳必须挡住。
  for (char c : path) {
    const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') || c == '.' || c == '_' ||
                    c == '/' || c == '-';
    if (!ok) return false;
  }
  return path.find("..") == std::string::npos;
}

}  // namespace usb
