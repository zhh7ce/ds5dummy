/**
 * @file audio_dump.cpp
 * @brief 离线回听入口：不起 USB/BT、不连手柄，只跑 audio 下行链并把产出写成音频文件。
 * @details 存在的理由是给"编出来的声音对不对"一个出口。daemon 里 audio 的产出只有
 *          一个去处（bridge），落盘不属于生产链路，所以这里拿 `tools::dump` 当
 *          `audio::OutputSink` 传进去——同一条链、同一个回调形状，只是接的人换了。
 *
 *          用法：`audio_dump <前缀>`，产出 `<前缀>.opus`（喇叭，ffplay/VLC 直接播）
 *          和 `<前缀>.haptics.wav`（触觉，3kHz 立体声 PCM U8）。Ctrl-C 收尾。
 *          audio 建的是被动 Audio/Sink 节点，没有应用把输出路由进来就没有数据，
 *          而且它不是默认设备——用 `wpctl set-default` 抢默认既没必要，也会把用户
 *          的正常音频一起改掉。要显式定向：
 *          @code
 *          # 节点名 = 本工具的 node.name（含本站 MAC 标识），pw-dump 里按 Audio/Sink 取
 *          NAME=$(pw-dump | jq -r '.[] | select(.info.props["media.class"]=="Audio/Sink")
 *                 | .info.props["node.name"] | select(test("DualSense"))' | head -1)
 *          pw-play --target="$NAME" sound.wav
 *          @endcode
 *          `--target` 也接受对象号，但那个数字会和别的节点的 serial 撞上：撞上时
 *          WirePlumber 按 serial 解析到别的节点、这头没连上，声音就从默认设备放出来
 *          （验证中 id 187 命中、id 124 未命中），因此只使用节点名。pavucontrol 中手动
 *          将输出流路由到该节点同样可行。
 *
 *          两个出口取自不同声道：`.opus` 是 ch0/ch1（FL/FR），`.haptics.wav` 是
 *          ch2/ch3（RL/RR，48kHz 重采样到 3kHz 后量化成 int8）。立体声文件经
 *          remix 后 ch2/ch3 通常是静音，验触觉要送 4 声道源（pw-play 按文件自带的
 *          声道数建流，`--channel-map FL,FR,RL,RR` 只在裸数据时才要写）。
 *          int8 只有约 48dB 动态范围，路由时避免把 `--volume` 压得过低：0.9 幅度的
 *          触觉信号压到 0.02 增益后小于 0.5/127 的量化步长，落盘结果会全为 0。
 */

#include "audio/audio.h"
#include "core/debug.h"
#include "loguru.hpp"
#include "tools/dump_sink.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <iostream>

namespace {

std::atomic<bool> stopping{false};

void on_signal(int) { stopping = true; }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "用法: " << argv[0] << " <输出前缀>\n"
                  << "  产出 <前缀>.opus 与 <前缀>.haptics.wav，Ctrl-C 收尾\n";
        return 2;
    }
    const std::string prefix = argv[1];
    // 级别自己定，loguru::init 只喂程序名——和 main.cpp 一样，不让它去扫 argv
    // 里的 -v（这里 argv[2..] 根本没有）。
    loguru::g_stderr_verbosity = loguru::Verbosity_INFO;
    char* init_argv[2] = {argv[0], nullptr};
    int init_argc = 1;
    loguru::init(init_argc, init_argv);

    if (!tools::dump::open(prefix)) {
        DS5DUMMY_LOG(ERROR, "[audio_dump] cannot open dump targets under {}", prefix.c_str());
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // Node name is derived from a MAC; this path has no controller, so all-zero
    // — it only shows up in wpctl/pavucontrol.
    constexpr const char* kDeviceId = "00:00:00:00:00:00";
    // Gain 1.0 = the controller would hear the mix as encoded; this path exists
    // to check the bytes, not a user's haptics preference.
    // Empty sysfs path: no gadget was provisioned here, so the sink carries no
    // device.sysfs.path — nothing in this tool consumes that property.
    if (!audio::Start(kDeviceId, 1.0f, tools::dump::write, "")) {
        DS5DUMMY_LOG(ERROR, "[audio_dump] audio downlink did not come up");
        tools::dump::close();
        return 1;
    }

    // 被动 sink 不路由就没有数据，而 --target 只认节点名才不会被别的节点的 serial
    // 抢走（见文件头）。节点名由 audio::identity 按本站标识拼出来，这里不重复那条
    // 规则——上面 `[sink] started: node=...` 那行日志里就是它。
    std::cout
        << "录制中，Ctrl-C 结束。把上面 [sink] started 里的 node=... 设为目标来放声音：\n\n"
        << "  pw-play --target='<那个 node 名>' <文件>\n\n"
        << "只听到喇叭侧：送立体声即可；触觉侧取 ch2/ch3，要验它得送 4 声道源。\n\n"
        << std::flush;
    while (!stopping) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 顺序是契约：先停编码线程，再收尾文件（close 之后不能再有 write）。
    audio::Stop();
    tools::dump::close();
    return 0;
}
