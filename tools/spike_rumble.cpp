// spike_rumble.cpp — 震动验证工具
//
// 目的：
//   1) hidraw 通道（对照组）发震动包 → 手柄应震动
//   2) L2CAP HIDP interrupt 通道（目标）发同一个震动包 → 手柄应震动
//   两边 CRC seed 相同 (0xEADA2D49)，payload 相同。
//
// 用法：
//   g++ -std=c++17 -Wall -o /tmp/spike_rumble tools/spike_rumble.cpp -lbluetooth
//   然后 /tmp/spike_rumble --bt 0C:27:56:82:70:5E
//
// 成功判定：运行时手柄明显震动 ~2s

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <linux/hidraw.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <inttypes.h>
#include <time.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <filesystem>

#include <string>
#include <vector>
#include <array>

// ---- DualSense constants ----
static constexpr uint16_t HIDP_PSM_CONTROL   = 0x11;
static constexpr uint16_t HIDP_PSM_INTERRUPT = 0x13;

// CRC-32 种子（和 ds5dummy types.h 一致）
static constexpr uint32_t CRC32_SEED_OUTPUT = 0xEADA2D49;

// BT output report 长度 78
static constexpr size_t BT_OUT_LEN = 78;

#define GREEN "\033[32m"
#define RED   "\033[31m"
#define YELLOW "\033[33m"
#define CYAN  "\033[36m"
#define RESET "\033[0m"

// ---------------------------------------------------------------------------
// CRC-32（必须与 src/core/protocol/crc32.h 的 Crc32() 一致；否则控制器会丢弃报告）
// 手柄会校验输出报告末尾 4 字节 CRC，不对就直接丢 send() 照样 OK。
// ---------------------------------------------------------------------------
static uint32_t crc32_out(const uint8_t* data, size_t len) {
    // types.h: crc = ~seed; 然后查表; return ~crc;
    // 多项式 0xEDB88320（CRC-32/IEEE reflected）
    static uint32_t table[256] = {};
    static bool inited = false;
    if (!inited) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int b = 0; b < 8; b++)
                c = (c >> 1) ^ (c & 1 ? 0xEDB88320u : 0u);
            table[i] = c;
        }
        inited = true;
    }
    uint32_t crc = ~CRC32_SEED_OUTPUT;   // ← 关键：取反 seed
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xff];
    }
    return ~crc;                          // ← 关键：结果再取反
}

// ---------------------------------------------------------------------------
// 构造完整 78 字节 BT output 报告
// [0x31][seq<<4|0x0][flags=0x10][SetStateData 47B][padding 24B][CRC32 4B]
//
// payload 需要让手柄真正处理震动——设多个 enable bit + MotorPowerLevel:
//   EnableRumbleEmulation  (byte[0].0)   = 1
//   UseRumbleNotHaptics     (byte[0].1)   = 1
//   AllowMotorPowerLevel    (byte[1].6)   = 1   ← 允许修改电机功率
//   AllowLedColor           (byte[1].2)   = 1   ← 允许改 LED 做视觉确认
//   AllowLightBrightness... (byte[38].0)  = 1   ← 允许亮度变化
//   RumbleEmulationRight    (byte[2])     = right (0-255)
//   RumbleEmulationLeft     (byte[3])     = left  (0-255)
//   RumbleMotorPowerReduction (byte[36]低4位) = 0xF (最大电机功率)
//   LedRed (byte[44]) = 255, LedGreen/LedBlue = 0 → 红色灯亮（视觉确认）
// ---------------------------------------------------------------------------
static std::array<uint8_t, BT_OUT_LEN> make_output_report(
    uint8_t seq, uint8_t right, uint8_t left)
{
    std::array<uint8_t, BT_OUT_LEN> pkt = {};

    pkt[0] = 0x31;                              // BT report type
    pkt[1] = (seq & 0x0F) << 4;                 // seq counter (高 4 bit)
    pkt[2] = 0x10;                              // flags: rumble enabled

    // SetStateData (47 字节, [3..49])
    pkt[3] = 0x03;   // byte[0]: EnableRumbleEmulation=1, UseRumbleNotHaptics=1
    pkt[4] = 0x44;   // byte[1]: AllowLedColor=1, AllowMotorPowerLevel=1
    pkt[5] = right;  // byte[2]: RumbleEmulationRight
    pkt[6] = left;   // byte[3]: RumbleEmulationLeft

    // byte[36] offset = 3+36 = 39
    pkt[39] = 0x0F;  // RumbleMotorPowerReduction = 0xF (最大电机功率)

    // byte[38] offset = 3+38 = 41
    pkt[41] = 0x01;  // AllowLightBrightnessChange = 1

    // LED 红色（视觉确认）: byte[44..46] offset = 3+44..46 = 47..49
    pkt[47] = 255;   // LedRed
    pkt[48] = 0;     // LedGreen
    pkt[49] = 0;     // LedBlue

    // [50..73] = padding（全 0）

    // CRC: 覆盖 [0..73]
    uint32_t crc = crc32_out(pkt.data(), 74);
    pkt[74] = static_cast<uint8_t>(crc & 0xFF);
    pkt[75] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    pkt[76] = static_cast<uint8_t>((crc >> 16) & 0xFF);
    pkt[77] = static_cast<uint8_t>((crc >> 24) & 0xFF);

    return pkt;
}

// ---------------------------------------------------------------------------
// 把 0x31 BT 报告 包装到 HIDP interrupt channel 上
// 格式：[0xA2][BT 报告 78 字节]  = 79 字节
// ---------------------------------------------------------------------------
static std::vector<uint8_t> wrap_interrupt_pkt(const uint8_t* bt_report, size_t n) {
    std::vector<uint8_t> pkt(1 + n);
    pkt[0] = 0xA2;
    memcpy(pkt.data() + 1, bt_report, n);
    return pkt;
}

// ---------------------------------------------------------------------------
// 找 DualSense hidraw fd
// ---------------------------------------------------------------------------
static int FindDualSenseHidraw() {
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/hidraw")) {
        std::filesystem::path uevent = entry.path() / "device" / "uevent";
        FILE* f = fopen(uevent.c_str(), "re");
        if (!f) continue;
        char line[256];
        uint32_t vid = 0, pid = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "HID_ID=", 7) == 0) {
                unsigned a, b, c;
                if (sscanf(line, "HID_ID=%x:%x:%x", &a, &b, &c) == 3) {
                    vid = b; pid = c;
                }
            }
        }
        fclose(f);
        if (vid == 0x054C && pid == 0x0CE6) {
            std::string name = entry.path().filename().string();
            std::string dev = "/dev/" + name;
            printf("%s[hidraw]%s 找到 DualSense: %s\n", GREEN, RESET, dev.c_str());
            int fd = ::open(dev.c_str(), O_RDWR);
            if (fd < 0) {
                printf("  open 失败: %s\n", strerror(errno));
            }
            return fd;
        }
    }
    printf("%s[hidraw]%s 没找到 DualSense\n", YELLOW, RESET);
    return -1;
}

// ---------------------------------------------------------------------------
// 打开 L2CAP interrupt channel
// ---------------------------------------------------------------------------
static int open_l2cap_intr(const bdaddr_t* ba) {
    int fd = ::socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    struct l2cap_options opts = {672, 672, 0, 0, 0, 0, 0};
    ::setsockopt(fd, SOL_L2CAP, L2CAP_OPTIONS, &opts, sizeof(opts));

    struct sockaddr_l2 sa = {};
    sa.l2_family = AF_BLUETOOTH;
    sa.l2_psm = HIDP_PSM_INTERRUPT;
    sa.l2_bdaddr = *ba;

    printf("%s[L2CAP]%s 连 PSM 0x13 interrupt...\n", CYAN, RESET);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0) {
        printf("  connect errno=%d (%s)\n", errno, strerror(errno));
        ::close(fd);
        return -1;
    }
    printf("%s[L2CAP]%s interrupt connected\n", GREEN, RESET);
    return fd;
}

// ---------------------------------------------------------------------------
// 震动测试：指定 fd + 标签，发 N 次震动后返回
// ---------------------------------------------------------------------------
static bool rumble_test(int fd, bool is_l2cap, const char* label, int duration_ms) {
    printf("%s=== 震动测试: %s ===%s\n", CYAN, label, RESET);

    for (int i = 0; i < 3; i++) {
        auto bt = make_output_report(static_cast<uint8_t>(i + 1), 255, 255);

        const uint8_t* send_ptr;
        size_t send_len;
        std::vector<uint8_t> wrapped;

        if (is_l2cap) {
            // L2CAP interrupt: [0xA2][BT report 78B] = 79B
            wrapped = wrap_interrupt_pkt(bt.data(), bt.size());
            send_ptr = wrapped.data();
            send_len = wrapped.size();
        } else {
            // hidraw: 直接写完整 BT report 78B（主程序 write_output_report 也这么做）
            // 不是 USB HID report 格式！hidraw 的 report ID 就是 BT report 首字节 0x31
            send_ptr = bt.data();
            send_len = bt.size();
        }

        printf("  #%d: send %zu bytes... ", i + 1, send_len);
        ssize_t n = is_l2cap ? ::send(fd, send_ptr, send_len, 0)
                             : ::write(fd, send_ptr, send_len);
        if (n < 0) {
            printf("%sFAIL errno=%d (%s)%s\n", RED, errno, strerror(errno), RESET);
            return false;
        }
        if (static_cast<size_t>(n) != send_len) {
            printf("%sWARN 只写了 %zd/%zu%s\n", YELLOW, n, send_len, RESET);
        } else {
            printf("%sOK%s (R=255 L=255, LED=RED)\n", GREEN, RESET);
        }
        usleep(static_cast<useconds_t>(duration_ms * 1000));
    }

    // 停震 + 灭 LED
    auto stop = make_output_report(10, 0, 0);
    // 清除 LED（offset 3+44..46 = 47..49）
    stop[47] = stop[48] = stop[49] = 0;
    auto update_crc = [](auto& pkt) {
        uint32_t crc = crc32_out(pkt.data(), 74);
        pkt[74] = static_cast<uint8_t>(crc & 0xFF);
        pkt[75] = static_cast<uint8_t>((crc >> 8) & 0xFF);
        pkt[76] = static_cast<uint8_t>((crc >> 16) & 0xFF);
        pkt[77] = static_cast<uint8_t>((crc >> 24) & 0xFF);
    };
    update_crc(stop);

    if (is_l2cap) {
        auto w = wrap_interrupt_pkt(stop.data(), stop.size());
        ::send(fd, w.data(), w.size(), 0);
    } else {
        ::write(fd, stop.data(), stop.size());
    }

    printf("  等 1s 让震动消失 + 灯灭...\n\n");
    usleep(1000 * 1000);
    return true;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    std::string mac_str;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bt") == 0 && i + 1 < argc) mac_str = argv[++i];
        else if (strcmp(argv[i], "--help") == 0) {
            printf("用法: %s --bt <MAC>\n", argv[0]);
            return 0;
        }
    }
    if (mac_str.empty()) {
        fprintf(stderr, "%s需指定 --bt <MAC>%s\n", RED, RESET);
        return 1;
    }

    bdaddr_t bdaddr;
    if (str2ba(mac_str.c_str(), &bdaddr) != 0) {
        fprintf(stderr, "%sMAC 解析失败%s\n", RED, RESET);
        return 1;
    }

    printf("%s========= DS5 Rumble 验证 =========%s\n", CYAN, RESET);
    printf("目标: %s\n", mac_str.c_str());
    printf("震动策略: R=255 L=255, 发 3 次每次 300ms, 然后停\n\n");

    // ---- 先做 hidraw 对照组 ----
    printf("%s--- Phase A: hidraw 对照组 ---\n%s", CYAN, RESET);
    int hw_fd = FindDualSenseHidraw();
    bool hidraw_ok = false;
    if (hw_fd >= 0) {
        hidraw_ok = rumble_test(hw_fd, false, "hidraw (对照组)", 300);
        ::close(hw_fd);
    } else {
        printf("%s[SKIP] hidraw 没找到 DualSense — 跳过对照组%s\n\n", YELLOW, RESET);
    }

    // ---- L2CAP interrupt 通道 ----
    printf("%s--- Phase B: L2CAP HIDP interrupt (目标) ---\n%s", CYAN, RESET);
    int l2_fd = open_l2cap_intr(&bdaddr);
    bool l2cap_ok = false;
    if (l2_fd >= 0) {
        l2cap_ok = rumble_test(l2_fd, true, "L2CAP interrupt", 300);
        ::close(l2_fd);
    } else {
        printf("%s[SKIP] L2CAP connect 失败%s\n\n", YELLOW, RESET);
    }

    // ---- 结论 ----
    printf("%s========= 结论 =========%s\n", CYAN, RESET);
    printf("  hidraw 震动: %s\n", hidraw_ok ? GREEN "OK (对照组确认手柄能震)" RESET : YELLOW "跳过" RESET);
    printf("  L2CAP 震动:  %s\n", l2cap_ok ? GREEN "OK — 手柄在 L2CAP 链路上震了！" RESET : RED "FAILED — L2CAP 包没让手柄震" RESET);

    if (l2cap_ok) {
        printf("\n%s*** SPIKE 完全成立：L2CAP HIDP 上 0xA2 0x31 带正确 CRC 能触发震动 ***%s\n", GREEN, RESET);
        printf("     重构方案 src/l2cap_channel.* 可以正式落地\n");
    } else {
        printf("\n%s*** SPIKE 部分失败：L2CAP 通道能写但震动没触发 ***%s\n", RED, RESET);
        printf("     需要查：是不是缺 feature init / 是不是需要先 GET_REPORT 0x09 再震 / 是不是 CRC 算法还要匹配 feature\n");
    }

    return (l2cap_ok ? 0 : 1);
}
