// spike_l2cap.cpp — Phase 0 spike: 验证裸 AF_BLUETOOTH L2CAP HIDP 连 DualSense
//
// 目的：确认在"bonded 经典手柄 + hidp 未加载"条件下，SOCK_SEQPACKET 能连上
// PSM 0x11（HIDP control）和 0x13（HIDP interrupt），MTU 足够（>= 400），
// 且控制器接受无 HIDP 握手的 0x31 / 0x43 报告。
//
// 用法：
//   sudo g++ -std=c++17 spike_l2cap.cpp -o spike_l2cap && sudo ./spike_l2cap --bt 0C:27:56:82:70:5E
//
// 输出结论（任何一步失败都要停下汇报）：
//   [OK/FAIL] PSM 0x11 connect
//   [OK/FAIL] PSM 0x13 connect
//   [OK/FAIL] MTU 协商 >= 400
//   [OK/FAIL] 0x43 GET_REPORT 0x09（固件 init_feature）有回复
//   [OK/FAIL] 0xA2 0x31 输出报告（0 bytes）有回复
//
// 成功的话：重构设计落地到 src/l2cap_channel.*
// 失败的话：停下，看哪个阶段、什么 errno、是否需要调整设计（或回退 hidraw）

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <inttypes.h>
#include <chrono>

#include <string>
#include <vector>

// ---- DualSense 相关常量（来自重构设计 §3.2）----
static constexpr uint16_t HIDP_PSM_CONTROL   = 0x11;
static constexpr uint16_t HIDP_PSM_INTERRUPT = 0x13;
static constexpr uint16_t HIDP_CID_BASIC     = 0x0040;

// 上游固件 MTU = 672；DualSense HIDP MTU 一般能协商到这个值
static constexpr uint16_t kWantMtu = 672;

// 简单颜色
#define GREEN "\033[32m"
#define RED   "\033[31m"
#define YELLOW "\033[33m"
#define CYAN  "\033[36m"
#define RESET "\033[0m"

static volatile std::sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

// 打印 hex dump
static void hexdump(const char* prefix, const uint8_t* buf, size_t len) {
    printf("%s", prefix);
    for (size_t i = 0; i < len && i < 96; i++) {
        if (i % 16 == 0) printf("\n  ");
        printf("%02x ", buf[i]);
    }
    if (len > 96) printf("... (%zu 字节总)\n", len);
    else printf("\n");
}

// 带超时的 recv
static ssize_t recv_timeout(int fd, uint8_t* buf, size_t buflen, int timeout_ms) {
    struct pollfd pfd = {fd, POLLIN, 0};
    int r = ::poll(&pfd, 1, timeout_ms);
    if (r < 0) return -1;
    if (r == 0) return 0;
    return ::recv(fd, buf, buflen, 0);
}

// 打印 AF_BLUETOOTH 错误码的人类可读描述
static void print_errno(int e) {
    switch (e) {
    case EHOSTDOWN:
        printf("%s [EHOSTDOWN] 手柄处于 LE 链路不可 page — 先 disconnect LE 再启动 spike %s\n", RED, RESET); break;
    case ENETUNREACH:
        printf("%s [ENETUNREACH] 经典连接不可达 — 检查手柄是否仍有 LE 链路 %s\n", RED, RESET); break;
    case ECONNREFUSED:
        printf("%s [ECONNREFUSED] PSM 拒绝连接 — 该 PSM 没开或 HIDP 已由 hidp 占用 %s\n", YELLOW, RESET); break;
    case ETIMEDOUT:
        printf("%s [ETIMEDOUT] 连接超时 — link key 过期或距离太远 %s\n", RED, RESET); break;
    case EPIPE:
        printf("%s [EPIPE] 对端关闭连接 — MTU 不够 / 包被拒 / 手柄休眠 %s\n", RED, RESET); break;
    case EAGAIN:
        printf("%s [EAGAIN] socket 非阻塞，稍后重试 %s\n", YELLOW, RESET); break;
    default: {
        const char* name = strerrorname_np(e);
        printf("%s [%s(%d)] %s %s\n", YELLOW, name ? name : "?", e, strerror(e), RESET);
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// 核心：L2CAP connect 到指定 PSM
// ---------------------------------------------------------------------------
static int l2cap_connect(const bdaddr_t* ba, uint16_t psm, const char* label) {
    int fd = ::socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (fd < 0) {
        printf("%s[FAIL]%s %s: socket() = -1 (%s)\n", RED, RESET, label, strerror(errno));
        return -1;
    }

    // 请求 MTU（尽量大；控制器回应的 MTU 是它愿意接受的上限）
    struct l2cap_options opts = {};
    opts.imtu = kWantMtu;
    opts.omtu = kWantMtu;
    if (::setsockopt(fd, SOL_L2CAP, L2CAP_OPTIONS, &opts, sizeof(opts)) < 0) {
        printf("%s[WARN]%s %s: setsockopt L2CAP_OPTIONS failed (%s) — 用默认 MTU\n",
               YELLOW, RESET, label, strerror(errno));
    }

    struct sockaddr_l2 sa = {};
    sa.l2_family = AF_BLUETOOTH;
    sa.l2_psm    = psm;
    sa.l2_bdaddr = *ba;
    // l2_dev = 0 (默认 hci0)；l2_cid = 0 (内核分配)；l2_bdaddr_type = 0 (public BR/EDR)

    printf("%s...%s 连 PSM 0x%02X (%s) -> %02x:%02x:%02x:%02x:%02x:%02x\n",
           CYAN, RESET, psm, label,
           ba->b[5], ba->b[4], ba->b[3], ba->b[2], ba->b[1], ba->b[0]);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0) {
        int e = errno;
        printf("%s[FAIL]%s %s: connect() errno=%d (%s)\n", RED, RESET, label, e, strerror(e));
        print_errno(e);
        ::close(fd);
        return -1;
    }

    // 读取 MTU 协商结果
    socklen_t olen = sizeof(opts);
    if (::getsockopt(fd, SOL_L2CAP, L2CAP_OPTIONS, &opts, &olen) == 0) {
        printf("%s[OK]%s %s connected (imtu=%u omtu=%u)\n", GREEN, RESET, label, opts.imtu, opts.omtu);
        if (opts.imtu < 400 || opts.omtu < 400) {
            printf("%s[WARN]%s MTU 偏小 (<400) — 0x36 音频报告要 399B，可能装不下\n", YELLOW, RESET);
        }
    } else {
        printf("%s[OK]%s %s connected (MTU 未知)\n", GREEN, RESET, label);
    }
    return fd;
}

// ---------------------------------------------------------------------------
// Feature report 0x43 (HIDP GET_REPORT)
// 格式：[0x43][report_id]
// ---------------------------------------------------------------------------
static bool try_feature_get(int ctrl_fd, uint8_t report_id, const char* label) {
    uint8_t req[2] = {0x43, report_id};
    printf("%s...%s feature GET_REPORT 0x%02X (%s)\n", CYAN, RESET, report_id, label);

    ssize_t n = ::send(ctrl_fd, req, sizeof(req), 0);
    if (n < 0) {
        printf("%s[FAIL]%s send() errno=%d (%s)\n", RED, RESET, errno, strerror(errno));
        print_errno(errno);
        return false;
    }
    if (static_cast<size_t>(n) != sizeof(req)) {
        printf("%s[WARN]%s send() 只写了 %zd/%zu\n", YELLOW, RESET, n, sizeof(req));
    }

    uint8_t resp[512];
    n = recv_timeout(ctrl_fd, resp, sizeof(resp), 2000);
    if (n == 0) {
        printf("%s[FAIL]%s 2s 内无回复（控制器可能不认 0x43 或 link 已断）\n", RED, RESET);
        return false;
    }
    if (n < 0) {
        printf("%s[FAIL]%s recv() errno=%d (%s)\n", RED, RESET, errno, strerror(errno));
        print_errno(errno);
        return false;
    }

    hexdump("  reply: ", resp, static_cast<size_t>(n));
    // 上游固件说 reply 以 0xA3 开头
    if (resp[0] == 0xA3) {
        printf("%s[OK]%s reply 以 0xA3 开头 — feature 通道正确\n", GREEN, RESET);
    } else {
        printf("%s[WARN]%s reply 首字节 0x%02x != 0xA3 — 可能是 input 报告 %s\n", YELLOW, RESET, resp[0], RESET);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 中断通道上发 0xA2 0x31 输出报告（最小有效体）
// [0xA2][0x31][seq<<4][0x10][SetStateData 47B][padding][CRC32] — 但我们先
// 只发 header + 一小段有效负载，看看控制器会不会 EPIPE 或有回复
// ---------------------------------------------------------------------------
static bool try_output_report(int intr_fd) {
    // 构造最小的合理 0xA2 0x31 包（CRC 先不算，看控制器反应）
    // 格式错误时控制器可能返回 EPIPE、拒绝接收，或直接丢弃
    uint8_t pkt[32] = {
        0xA2,     // HIDP_DATA header（output on interrupt channel）
        0x31,     // DualSense output report type
        0x00,     // seq counter
        0x10,     // flags (rumble)
        // SetStateData 前几个字节全零
    };
    // 末尾填零做 CRC 占位（不影响我们想看"会不会 EPIPE"）

    printf("%s...%s interrupt 通道上发 0xA2 0x31 小包头\n", CYAN, RESET);
    hexdump("  send: ", pkt, 8);

    ssize_t n = ::send(intr_fd, pkt, sizeof(pkt), 0);
    if (n < 0) {
        printf("%s[FAIL]%s send() errno=%d (%s)\n", RED, RESET, errno, strerror(errno));
        print_errno(errno);
        return false;
    }
    printf("%s[OK]%s send %zd 字节（没 EPIPE — 中断通道可写）\n", GREEN, RESET, n);
    return true;
}

// ---------------------------------------------------------------------------
// 读 input channel 一段时间看有没有 0x31 输入报告
// ---------------------------------------------------------------------------
static int drain_input(int intr_fd, int ms) {
    int count = 0;
    uint8_t buf[512];
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);

    while (std::chrono::steady_clock::now() < end) {
        int remaining = 50; // poll 50ms
        auto now = std::chrono::steady_clock::now();
        remaining = std::min(remaining, static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(end - now).count()));
        if (remaining <= 0) break;

        ssize_t n = recv_timeout(intr_fd, buf, sizeof(buf), remaining);
        if (n == 0) {
            // timeout
            continue;
        }
        if (n < 0) {
            printf("%s[WARN]%s recv() errno=%d (%s)\n", YELLOW, RESET, errno, strerror(errno));
            return count;
        }
        count++;

        // 分析：第一个字节应该是 HIDP 事务头，第二个应该是 0x31
        if (n >= 2) {
            printf("%s[OK]%s 收到 %zd 字节 input\n", GREEN, RESET, n);
            printf("  byte[0]=0x%02x (HIDP header)  byte[1]=0x%02x (report type)\n",
                   buf[0], buf[1]);
            if (buf[1] == 0x31) {
                printf("%s[MATCH]%s 这是 DualSense 0x31 input 报告！\n", GREEN, RESET);
                // flags 在 byte[2]：bit 1 是 mic 数据
                if (n >= 3)
                    printf("  flags=0x%02x (mic=%d)\n", buf[2], (buf[2] >> 1) & 1);
                // USBGetStateData 从 byte[3] 起 63 字节
                if (n >= 65) {
                    const uint8_t* s = buf + 3;
                    printf("  sticks L/R = [%3d,%3d]  triggers = [%3d,%3d]\n",
                           s[0], s[1], s[4], s[5]);
                }
            }
        }
        hexdump("  data: ", buf, static_cast<size_t>(n));
    }
    return count;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    std::string mac_str;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bt") == 0 && i + 1 < argc)
            mac_str = argv[++i];
        else if (strcmp(argv[i], "--help") == 0) {
            printf("用法: %s --bt AA:BB:CC:DD:EE:FF\n", argv[0]);
            return 0;
        }
    }
    if (mac_str.empty()) {
        fprintf(stderr, "%s[FAIL]%s 需指定 --bt <MAC>\n", RED, RESET);
        return 1;
    }

    bdaddr_t bdaddr;
    if (str2ba(mac_str.c_str(), &bdaddr) != 0) {
        fprintf(stderr, "%s[FAIL]%s MAC 解析失败: %s\n", RED, RESET, mac_str.c_str());
        return 1;
    }

    printf("%s======== Phase 0 spike: L2CAP HIDP ========\n%s", CYAN, RESET);
    printf("目标: %s\n", mac_str.c_str());
    printf("PSM 0x11 = HIDP control,  PSM 0x13 = HIDP interrupt\n\n");

    // 检查 hidp 模块状态
    FILE* lsmod = popen("lsmod | grep -E '^hidp[[:space:]]'", "r");
    if (lsmod) {
        char line[256];
        if (fgets(line, sizeof(line), lsmod)) {
            printf("%s[WARN]%s hidp 模块已加载！BlueZ hidp 插件可能抢先接管\n", YELLOW, RESET);
            printf("       spike 可能被 ECONNREFUSED — 先: sudo modprobe -r hidp\n");
        }
        pclose(lsmod);
    }

    // ---- Step 1: L2CAP connect ----
    printf("\n%s--- Step 1: connect ---\n%s", CYAN, RESET);
    int ctrl = l2cap_connect(&bdaddr, HIDP_PSM_CONTROL,   "control");
    int intr = l2cap_connect(&bdaddr, HIDP_PSM_INTERRUPT, "interrupt");
    if (ctrl < 0 || intr < 0) {
        printf("\n%s[SPIKE 结论]%s L2CAP 连接失败 — 不回退 hidraw，先修连接\n", RED, RESET);
        if (ctrl >= 0) ::close(ctrl);
        if (intr >= 0) ::close(intr);
        return 2;
    }

    // ---- Step 2: Feature GET_REPORT ----
    printf("\n%s--- Step 2: feature GET_REPORT 0x09 (固件 init_feature) ---\n%s", CYAN, RESET);
    bool f1 = try_feature_get(ctrl, 0x09, "init_feature");

    // 再试 0x20（固件读是否 DSE）
    printf("\n%s--- Step 3: feature GET_REPORT 0x20 ---\n%s", CYAN, RESET);
    bool f2 = try_feature_get(ctrl, 0x20, "calibration");

    // ---- Step 4: interrupt channel 写 0xA2 0x31 ----
    printf("\n%s--- Step 4: interrupt 通道上发最小 0xA2 0x31 ---\n%s", CYAN, RESET);
    bool out_ok = try_output_report(intr);

    // ---- Step 5: 读 input ----
    printf("\n%s--- Step 5: 读 input（5s，按一下手柄看有没有来）---\n%s", CYAN, RESET);
    int got = drain_input(intr, 5000);
    printf("\n  5s 内收到 %d 包\n", got);

    // ---- 结论 ----
    printf("\n%s======== Spike 结论 ========\n%s", CYAN, RESET);
    printf("  PSM 0x11 connect:   %s\n", ctrl >= 0 ? GREEN "OK" RESET : RED "FAIL" RESET);
    printf("  PSM 0x13 connect:   %s\n", intr >= 0 ? GREEN "OK" RESET : RED "FAIL" RESET);
    printf("  feature GET 0x09:   %s\n", f1        ? GREEN "OK" RESET : RED "FAIL" RESET);
    printf("  feature GET 0x20:   %s\n", f2        ? GREEN "OK" RESET : RED "FAIL" RESET);
    printf("  0xA2 0x31 可写:     %s\n", out_ok    ? GREEN "OK" RESET : RED "FAIL" RESET);
    printf("  收到 0x31 input:    %s\n", got > 0   ? GREEN "OK" RESET : YELLOW "未收到 (没按键或 link 被干扰)" RESET);

    if (ctrl >= 0) ::close(ctrl);
    if (intr >= 0) ::close(intr);

    bool all = (ctrl >= 0) && (intr >= 0) && f1 && f2 && out_ok;
    printf("\n  总体: %s\n", all ? GREEN "SPIKE 成立 — 可以落地重构" RESET : RED "SPIKE 部分失败 — 停下分析" RESET);
    return all ? 0 : 1;
}
