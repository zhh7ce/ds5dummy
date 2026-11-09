/**
 * @file
 * @brief buttons 插件的 id 空间与 63 字节状态体布局
 *
 * 这一层是插件里唯一认识字节形状的地方：可绑定源与轴的 id 空间、配置 token 与
 * id 的对应表、状态体与 Frame 快照之间的编解码。它不认识配置文本的语法
 * （settings），不认识规则（rules），也不持有任何状态。
 *
 * @note 偏移与位映射是 core/protocol/input_state.cpp 那张解码表的第二处写法：
 *       插件只 include plugin_api.h，拿不到宿主的协议头，所以这里必须自己编排
 *       一遍。两份实现由 buttons_ids_test.cpp 逐字段对账，改布局时漏改哪一边都
 *       会在那里失败，而不是变成"按 ○ 出 ●"这种静默错误。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "plugin/plugin_api.h"

namespace buttons {
namespace ids {

/**
 * @brief 可以出现在配置里的源。
 *
 * @details 十字键在这里是四个方向位，不是线上那个 hat 值：hat 的 0..7 恰好是
 *          四个方向的全部合法组合、8 是未按下，两者无损对应（见 EncodeFrame），
 *          所以"按位合并"就是需求 M-10/M-11 要的"按轴合并、只抹掉某一个方向"。
 * @note 摇杆的八个方向位与触摸板的那九个位**都不来自** DecodeFrame：它们是判出来
 *       的，不是线上有的位，而且必须带迟滞或锁存才能避免"停在阈值上的摇杆一帧开
 *       一帧关"（I-01）、"手指滑过中线时半区归属来回跳"（I-03）。这里列进源空间
 *       只是因为配置里的 token 与位要一张共同的表；解出方向位的是
 *       StickDirections()，解出触摸板那族的是 TouchpadStates()。
 */
enum class Source : uint8_t {
  kDpadUp,       ///< 十字键上。
  kDpadRight,    ///< 十字键右。
  kDpadDown,     ///< 十字键下。
  kDpadLeft,     ///< 十字键左。
  kTriangle,     ///< △。
  kCircle,       ///< ○。
  kCross,        ///< ✕。
  kSquare,       ///< □。
  kL1,           ///< L1。
  kR1,           ///< R1。
  kL2,           ///< L2 的数字位；模拟量是 Axis::kLTrigger，两者是同一个键的两半。
  kR2,           ///< R2 的数字位；模拟量是 Axis::kRTrigger。
  kCreate,       ///< Create。
  kOptions,      ///< Options。
  kL3,           ///< 左摇杆按下。
  kR3,           ///< 右摇杆按下。
  kHome,         ///< Home/PS。
  kPad,          ///< 触摸板按下。
  kMute,         ///< Mute。
  kLFn,          ///< 左 Fn（Edge）。
  kRFn,          ///< 右 Fn（Edge）。
  kLPaddle,      ///< 左背键（Edge）。
  kRPaddle,      ///< 右背键（Edge）。
  kLstickUp,     ///< 左摇杆上（I-01）。
  kLstickRight,  ///< 左摇杆右。
  kLstickDown,   ///< 左摇杆下。
  kLstickLeft,   ///< 左摇杆左。
  kRstickUp,     ///< 右摇杆上。
  kRstickRight,  ///< 右摇杆右。
  kRstickDown,   ///< 右摇杆下。
  kRstickLeft,   ///< 右摇杆左。
  kTouchLeft,    ///< 有触点起始于触摸板左半（I-03 的半区，也是 I-04 的"起始半区"）。
  kTouchRight,   ///< 有触点起始于右半。
  kTouchTwo,     ///< 两个触点同时存在（I-04 的"一到两指"里的那一档）。
  kPadLeft,      ///< 左半点击（I-03）：触摸板按下位 + 左半那一侧有起始触点。
  kPadRight,     ///< 右半点击。
  kSwipeUp,      ///< 向上划动（I-04）。事件位：判出即置位，保持到所有触点离开，
                 ///< 一次手势只发一次，所以没有"按住"形态。
  kSwipeRight,   ///< 向右划动。
  kSwipeDown,    ///< 向下划动。
  kSwipeLeft,    ///< 向左划动。
  kCount,        ///< 上面这些的个数，也是 token 表的宽度。
};

/**
 * @brief 一组源的位集合。
 * @note 用 64 位而不是 32 位：23 个线上位（十字键按四个方向计）加 8 个摇杆方向
 *       占掉 31 位，触摸板那一族（I-03/I-04）再加 9 位，于是 32 位这一档已经用完。
 *       再往后的形态（六轴手势 I-05）要占几格没算过，这一位宽不是按它设计的。
 */
using Mask = uint64_t;

/**
 * @brief 一个源对应的位。
 * @param src 可绑定源。
 * @return 只含有这一位的掩码。
 */
constexpr Mask BitOf(Source src) {
  return static_cast<Mask>(1) << static_cast<uint8_t>(src);
}

/// 六根轴：摇杆四根加两个扳机，就是需求 G-03 里"轴"的全部内容。
enum class Axis : uint8_t {
  kLstickX,     ///< 左摇杆 X。
  kLstickY,     ///< 左摇杆 Y。
  kRstickX,     ///< 右摇杆 X。
  kRstickY,     ///< 右摇杆 Y。
  kLTrigger,    ///< 左扳机行程。
  kRTrigger,    ///< 右扳机行程。
  kCount,
};

/// 轴的根数。
constexpr uint8_t kAxisCount = static_cast<uint8_t>(Axis::kCount);

/**
 * @brief 同一根摇杆的另一根轴。
 * @param axis 摇杆的四根轴之一。
 * @return X 给 Y、Y 给 X。
 * @details "把左摇杆居中"永远是两根轴一起动，而方向位只告诉调用方它命中了其中
 *          一根。四根摇杆轴在 Axis 里按 X、Y 相邻排列，所以配对就是最低位取反；
 *          下面那条 static_assert 把这个前提钉住。
 */
constexpr Axis StickOther(Axis axis) {
  static_assert(static_cast<uint8_t>(Axis::kLstickY) ==
                    static_cast<uint8_t>(Axis::kLstickX) + 1 &&
                    static_cast<uint8_t>(Axis::kRstickY) ==
                        static_cast<uint8_t>(Axis::kRstickX) + 1,
                "each stick's X and Y must be adjacent in Axis");
  return static_cast<Axis>(static_cast<uint8_t>(axis) ^ 1u);
}

/**
 * @brief 另一根摇杆上同一方向的轴。
 * @param axis 摇杆的四根轴之一。
 * @return 左给右、右给左：X 对 X，Y 对 Y。
 * @details `swap_lstick_rstick` 说的就是这一对：整根换是两对，按轴换是一对。
 *          四根摇杆轴按左 X、左 Y、右 X、右 Y 排列，所以配对是加减 2；
 *          ids::StickOther 那条 static_assert 钉的就是同一个前提。
 */
constexpr Axis SwapPartner(Axis axis) {
  static_assert(static_cast<uint8_t>(Axis::kRstickX) ==
                    static_cast<uint8_t>(Axis::kLstickX) + 2,
                "the two sticks must be laid out as adjacent X/Y pairs");
  return static_cast<Axis>(static_cast<uint8_t>(axis) <
                                    static_cast<uint8_t>(Axis::kRstickX)
                                ? static_cast<uint8_t>(axis) + 2
                                : static_cast<uint8_t>(axis) - 2);
}

/**
 * @brief 一根轴在 Frame::axis 里的下标。
 * @param axis 六根轴之一。
 * @return 下标；Axis::kCount 落在数组末尾之外，调用方必须先比较再取。
 */
constexpr std::size_t IndexOf(Axis axis) {
  return static_cast<std::size_t>(static_cast<uint8_t>(axis));
}

/// 摇杆静置点在线上那个字节里的值。
constexpr uint8_t kStickCenter = 128;

/// 摇杆两端的线上值。
constexpr uint8_t kStickMin = 0;
constexpr uint8_t kStickMax = 255;

/// 扳机完全松开的线上值。
constexpr uint8_t kTriggerReleased = 0;

/// 扳机完全按下的线上值，也是"非扳机的键驱动扳机"时的全按值（M-08）。
constexpr uint8_t kTriggerPressed = 255;

/**
 * @brief 摇杆方向的缺省触发阈值与回迟滞（I-01），单位是"离中心多少个线上刻度"。
 * @details 经验值，与上游 Studio 的 MACRO_STICK_THRESH / MACRO_STICK_HYST 同值
 *          （48 与 10，都以 127 为满偏）。迟滞存在的理由只有一条：摇杆停在阈值
 *          上时，没有回迟滞就会一帧判有、一帧判无。
 */
constexpr uint8_t kStickStep = 48;
constexpr uint8_t kStickHysteresis = 10;

/**
 * @brief 触摸板的几何（I-03/I-04 的判据全部说在这里）。
 *
 * @details 两个坐标在线上都是 12 位、都跑满 0..4095，而面板实物是 16:9，所以
 *          **同一个 raw 刻度在横向与纵向代表的距离不一样**：判划动方向与长度时
 *          先把两路位移折算到同一刻度上（kPadAspectX/kPadAspectY 就是那个比）。
 * @note 面板的宽高比取自手柄的触摸板分辨率（本项目 DKMS 驱动里的
 *       DS_TOUCHPAD_WIDTH/HEIGHT 就是 1920/1080，约掉 120 得 16:9）。坐标本身只有
 *       12 位这一个事实来自线上格式，与 core 的 DecodeFinger 同源。
 */
/// 一个坐标轴的 raw 刻度数：12 位，0..4095。
constexpr uint32_t kPadSpan = 1u << 12;

/// x 的中线，左半与右半的分界。
constexpr uint32_t kPadMidX = kPadSpan / 2;

/// 中线两侧这条带算"任意半区"，不算某一侧（I-03）：宽度的 8%。
constexpr uint32_t kPadNeutralBand = kPadSpan * 8 / 100;

/// 纵向位移折算到横向刻度的比例分子与分母（1920:1080）。
constexpr uint32_t kPadAspectX = 16;
constexpr uint32_t kPadAspectY = 9;

/// 划动判定的最小位移，用横向刻度说：宽度的 12%。经验值——比半区那条带放宽，
/// 因为一次擦碰的位移通常就在 8% 那一档附近。
constexpr uint32_t kSwipeMinSpan = kPadSpan * 12 / 100;

/// 一份报告里的触摸点个数，线上就是那两个 32 位打包字。
constexpr std::size_t kTouchPoints = 2;

/**
 * @brief 一个触摸点在状态体里的那 32 位。
 * @details 只留这三个字段：序号那 7 位（bit0..6）手柄报的是它自己的触点编号，
 *          与"第几个触点"不是一件事，插件判半区与划动都不需要它。
 */
struct Contact {
  bool touching = false;   ///< 线上 bit7 是"未接触"，这里取反成"接触"。
  uint16_t x = 0;          ///< 12 位横坐标，0..4095。
  uint16_t y = 0;          ///< 12 位纵坐标，0..4095，向下增长。
};

/**
 * @brief 一个触摸点在这一轮的跨帧轨迹：半区在什么时候定下、划动从哪儿起算。
 * @details 半区与起点都只在这个点**刚出现**的那一帧写入，之后原样保留到它离开
 *          （I-03 的"按下那一刻定下、保持到松开"）：手指滑过中线时归属来回跳，
 *          表现就是左半点击一会儿变右半点击。
 * @note swiped 是"这一轮已经发过一次划动"，不是"这一帧还在划"。一次手势只发一个
 *       事件，第二下要等这个触点离开再上。
 */
struct TouchTrack {
  bool present = false;    ///< 上一帧这个触点在不在面板上。
  Mask side = 0;           ///< 出现那一刻定下的半区位：kTouchLeft、kTouchRight 或 0。
  uint16_t start_x = 0;    ///< 出现那一刻的横坐标。
  uint16_t start_y = 0;    ///< 出现那一刻的纵坐标。
  bool swiped = false;     ///< 这一轮已经判出过划动。
};

/**
 * @brief 触摸板那一族的跨帧状态。
 * @note swipe_latch 存在这里而不是每个触点各存自己的：划动位要"保持到所有触点
 *       离开"，而单个触点什么时候离开是它自己的事。
 */
struct TouchState {
  TouchTrack contact[kTouchPoints];
  Mask swipe_latch = 0;
};

/**
 * @brief 一份状态体里本插件认识的那些值：插件内部唯一的输入形态。
 * @details 它是跨帧只读的快照——一帧之内所有取值都从这里读，改写结果只在最后一
 *          趟写回缓冲区，所以链式映射不叠加（M-06）与"同帧原始值"（M-15）是结构
 *          性的，不靠纪律维持。
 * @note digits 里只有线上真存在的位：数字键与十字键的四个方向位。摇杆方向位不在
 *       这里（它们要迟滞，是判出来的），触摸板那一族也不在（半区要锁存、划动要
 *       比较位移），两者都由 sense 补齐之后再交给 pipeline。
 */
struct Frame {
  Mask digits = 0;                       ///< 线上那 23 个位（十字键是四个方向位）。
  uint8_t axis[kAxisCount] = {};         ///< 六根轴的原始字节。
  Contact contact[kTouchPoints] = {};    ///< 两个触摸点，线上 [32..35] 与 [36..39]。
};

/**
 * @brief 配置 token → 源。
 * @param token 键名原文（snake_case，大小写敏感，没有别名）。
 * @param out 命中时写入对应的源。
 * @return 命中 true；不在表里 false（out 不改动）。
 */
bool ParseToken(std::string_view token, Source& out);

/**
 * @brief 配置 token → 轴。
 * @param token 轴名原文，如 lstick_x、rtrigger。
 * @param out 命中时写入对应的轴。
 * @return 命中 true；不在表里 false。
 */
bool ParseAxisToken(std::string_view token, Axis& out);

/**
 * @brief 配置 token → 整根摇杆。
 * @param token 摇杆名原文：`lstick` 或 `rstick`，不带下标后缀。
 * @param out 命中时写入这根摇杆的 **X 轴**；Y 轴由 ids::StickOther 给出。
 * @return 命中 true；不在表里 false。
 * @details 轴名表里的四根摇杆轴都带 `_x`/`_y` 后缀，而"把左摇杆居中"说的是整根，
 *          所以配置里存在第二种粒度：这一张表就是它。扳机没有复数形态，因此
 *          `ltrigger` 在这里查不到。
 */
bool ParseStickToken(std::string_view token, Axis& out);

/**
 * @brief 源对应的 token 原文，用于报错回显和表对账。
 * @param src 可绑定源。
 * @return 表里那一项；src 越界时为空串。
 */
const char* TokenOf(Source src);

/**
 * @brief 轴对应的 token 原文。
 * @param axis 六根轴之一。
 * @return 表里那一项；axis 越界时为空串。
 */
const char* TokenOf(Axis axis);

/**
 * @brief 把 63 字节状态体解码成快照。
 * @param state 指向 kPluginStateSize 字节的状态体（不含 USB report ID）。
 * @return 本插件认识的那些值。
 * @note 十字键的 hat 在 9..15 时不产生任何方向位（与 kNone 同列）：那几个值手柄
 *       不会报，读到了也没有对应的方向组合，归一成"未按"比保留更少见。
 */
Frame DecodeFrame(const uint8_t* state);

/**
 * @brief 把快照写回 63 字节状态体。
 * @param frame 要写出的值。
 * @param state 目标缓冲区：本函数恰好写 [0..5]（六根轴）、[7] 的两个 nibble、
 *              [8] 的八个位和 [9] 里本插件拥有的七个位。
 * @note DecodeFrame 与它**不是**彼此的逆：[6] 的序号、[9] bit3 那个未解释的位、
 *       [10..62] 的传感器/触摸板/CMAC 一律不碰——插件不生产的字节原样留着，
 *       所以读进来再写回去时整份 63 字节逐字节相同（由单测断言）。
 */
void EncodeFrame(const Frame& frame, uint8_t* state);

/**
 * @brief 扳机键位对应的那根轴。
 * @param src 可绑定源。
 * @return L2 给 kLTrigger、R2 给 kRTrigger，其余源给 Axis::kCount 表示"没有轴"。
 * @details 需求 M-16 说扳机的模拟量与数字位是同一个键的两半：两半靠这一个映射
 *          互相找到对方，gate 掉数字位时才知道该把哪根轴清零。
 */
Axis TriggerAxisOf(Source src);

/**
 * @brief 这个源在状态体里有没有自己的位。
 * @param src 可绑定源。
 * @return 十字键的四个方向与 19 个键位为 true；八个摇杆方向与触摸板那九个为 false。
 * @details 两种用途：目标只能是线上真存在的位（把宏的目标写成 `lstick_up` 或
 *          `pad_left` 都没有可表达的形态），连点也只能配在这样的位上（T-03 排除
 *          扳机与摇杆，扳机由调用方另外排除，因为它还有轴那一半）。
 * @note 判据是枚举里的位置：八个方向与触摸板那一族都排在 23 个线上位之后，所以
 *       这里比较的是顺序而不是表。下面那条 static_assert 把这个前提钉住——改动
 *       枚举顺序会在这里失败。
 */
constexpr bool IsWireBit(Source src) {
  static_assert(static_cast<uint8_t>(Source::kRPaddle) + 1 ==
                    static_cast<uint8_t>(Source::kLstickUp),
                "every judged source must come after the 23 wire bits");
  return static_cast<uint8_t>(src) < static_cast<uint8_t>(Source::kLstickUp);
}

/**
 * @brief 这个源是不是判出来的摇杆方向（I-01 那八个）。
 * @param src 可绑定源。
 * @return 八个方向之一时 true。
 * @details 触摸板那一族紧挨在它们后面，同样是"判出来、不在线上"的位，sense 里
 *          那圈"命中期间把这根摇杆居中"的遍历因此不能连它们一起走：
 *          StickAxisOf 对它们返回 Axis::kCount，居中就会落到一根不存在的轴上。
 */
constexpr bool IsStickDirection(Source src) {
  static_assert(static_cast<uint8_t>(Source::kRstickLeft) + 1 ==
                    static_cast<uint8_t>(Source::kTouchLeft),
                "the eight stick directions must stay a contiguous block");
  const uint8_t index = static_cast<uint8_t>(src);
  return index >= static_cast<uint8_t>(Source::kLstickUp) &&
         index <= static_cast<uint8_t>(Source::kRstickLeft);
}

/**
 * @brief 这个源是不是一次划动（I-04 那四个）。
 * @param src 可绑定源。
 * @return 四个划动方向之一时 true。
 * @details 划动是一次性事件，没有"按住"的形态，所以它只能当触发的项与宏的项，
 *          不能当 remap 的源：remap 的"按住出目标位"在事件位上等于把一次手势
 *          拉长成一个持续好几帧的按键。这一条由 settings 在语法层拒绝。
 */
constexpr bool IsSwipe(Source src) {
  static_assert(static_cast<uint8_t>(Source::kSwipeLeft) + 1 ==
                    static_cast<uint8_t>(Source::kCount),
                "the four swipe directions must be the last sources");
  const uint8_t index = static_cast<uint8_t>(src);
  return index >= static_cast<uint8_t>(Source::kSwipeUp) &&
         index < static_cast<uint8_t>(Source::kCount);
}

/**
 * @brief 换掉一个源时，状态体里要抹掉哪些位。
 * @param src 映射的源。
 * @return 线上位的掩码；判出来的位没有对应字节，返回 0。
 * @details 只有左/右半点击是个例外：它们自己的位不在线上，但它们就是"触摸板按下
 *          位 + 半区"，要换掉左半点击却不抹掉 pad 那一位，主机仍然看到触摸板被
 *          按了一下。八个摇杆方向不需要这种展开——游戏读的是轴值，而轴那边由
 *          AxisRewrite 与 M-09 那套走。
 * @note 唯一的使用点是 pipeline::Apply 的 gate；判定层不看它，它读的始终是位。
 */
constexpr Mask GateBitsOf(Source src) {
  if (IsWireBit(src)) return BitOf(src);
  if (src == Source::kPadLeft || src == Source::kPadRight) {
    return BitOf(Source::kPad);
  }
  return 0;
}

/**
 * @brief 摇杆方向对应的那根轴。
 * @param src 可绑定源。
 * @return 八个方向源给各自的轴，其余源给 Axis::kCount 表示"不是方向"。
 */
Axis StickAxisOf(Source src);

/**
 * @brief 八个摇杆方向位：把快照里那四根摇杆轴判成方向（I-01）。
 * @param frame 快照（读的是它的原始轴值，所以 M-17 的"读物理摇杆"在这里自动成立）。
 * @param step 触发阈值，离中心的刻度数。
 * @param hysteresis 回迟滞：已经判出的方向要退到 step - hysteresis 才算松开。
 * @param latched 上一帧判出的方向位。
 * @return 这一帧判出的方向位集合。
 * @details 判据是每根轴一个带符号的投影：正方向是 x 的右与 y 的下（线上 y 向下
 *          增长，所以"上"是负值——这一点与上游 Studio 的 stick_dir_active() 一致）。
 *          已经锁存的方向用松开阈值判，未锁存的用触发阈值判，因此停在阈值上的摇杆
 *          不会一帧开一帧关；跨过中心之后投影变负，锁存自然解除。
 * @note 调用方负责把返回值存回跨帧状态（本函数不持状态，所以它可以在单测里被
 *       一帧一帧地喂）。方向位从不写进状态体：它们不是线上的位。
 */
Mask StickDirections(const Frame& frame, uint8_t step, uint8_t hysteresis,
                     Mask latched);

/**
 * @brief 触摸板那一族的位：半区、触点数、左/右半点击、四个划动方向（I-03/I-04）。
 * @param frame 这一帧的快照（读它的 digits 里的 pad 位与那两个触摸点）。
 * @param io 触摸板的跨帧状态，读写。
 * @return 这一帧该交给判定层的触摸板位集合：电平的那几种（`kTouchLeft`、
 *         `kTouchRight`、`kTouchTwo`、`kPadLeft`、`kPadRight`）说的是"现在就成立"，
 *         划动位说的是"这一轮手势里划出去过，还没松手"。
 * @details 半区与划动起点都锁存在触点上（见 TouchTrack），所以三件事自然成立：
 *          手指滑过中线不会改判半区、划动不会在按住不放时反复触发、触点全离开
 *          之后一切归零并从下一次接触重新定半区。
 * @note 与 StickDirections 同一条约定：本函数不持有状态，跨帧那一份由调用方存，
 *       因此可以在单测里逐帧喂。判出来的位从不写进状态体。
 * @note 只读不改 digits：这一帧改写过的字节不会喂回给自己（K-07）。
 */
Mask TouchpadStates(const Frame& frame, TouchState& io);

/**
 * @brief 一根轴反向（M-13），并且让静置点仍在静置（M-14）。
 * @param value 线上值 0..255。
 * @return 256 - value，0 那一端夹到 255。
 * @details 写成 255 - value 会让中心 128 变成 127：静置的摇杆反向之后是一根永远
 *          在缓慢输入的摇杆。夹取而不是回绕：中心那个不动点由 256 - 128 = 128
 *          给出，满偏 255 落到 1。
 * @note 代价是 0 与 1 都落到 255，所以这个映射在 0 那一端不是回返的。这是这一族
 *       映射固有的，不是实现没做干净：一个以 128 为唯一不动点的对合要把剩下 255
 *       个点两两配对，而 255 是奇数。要么静置点漂移，要么一端丢一个刻度，本函数
 *       按 M-14 选后者。
 */
uint8_t Invert(uint8_t value);

}  // namespace ids
}  // namespace buttons
