/**
 * @file
 * @brief 跨帧的触发判定：这次按下算不算命中、连点这一帧是开还是关、宏还剩几毫秒
 *
 * sense 只回答"这一帧该发生什么"，不写那 63 个字节一个字节：它拿到的是快照，
 * 交出去的是两组位和几个强制值，写回缓冲区是 pipeline 末尾那一趟的事。
 *
 * 时刻由调用方注入（`on_state_in` 取一次），所以这一层可以在单测里被一帧一帧地
 * 喂虚拟时钟，不需要只在测试构建里存在的拨表入口。
 */

#pragma once

#include <cstdint>
#include <vector>

#include "ids.h"
#include "rules.h"

namespace buttons {
namespace sense {

/**
 * @brief 一帧的判定结果。
 * @details 交给 pipeline 的是集合而不是命令：pipeline 不需要知道某个注入来自宏还是
 *          来自别的什么，两者的形状相同（一段时间内按住一组位）。
 */
struct FrameDecision {
  /**
   * @brief 这一帧的"物理"位：线上那 23 位，加上判出来的摇杆方向位与触摸板那族，
   *        再把连点的"关"半周期抹掉。
   * @details 这就是映射层看到的输入。连点作用在物理按键上、映射作用在结果上
   *          （T-04）在这里就是"塑形在映射之前"；而触发判定读的是塑形之前的
   *          那份（K-07），所以两个读取点各拿各的，互不污染。
   */
  ids::Mask digits = 0;

  /// 要强行按下的位：宏脉冲期间的那一组。这些位不参与映射。
  ids::Mask inject = 0;

  /**
   * @brief 被强制取值的轴（`宏全按` 与 `方向命中期间把摇杆居中`）。
   * @details 下标由 ids::IndexOf 给出。pipeline 的优先级里这一档最高，压过映射
   *          带来的行程与互换。
   */
  uint8_t forced_axis[ids::kAxisCount] = {};
  bool forces_axis[ids::kAxisCount] = {};

  /// 这一帧有没有命中一条关机动作。本模块从不执行它（K-10）。
  bool power_off = false;
};

/**
 * @brief 一条触发（组合键或宏）的跨帧状态。
 * @note 哪些字段服务于哪种时序形态写在 sense.cpp 的状态机里；这里不做成 union，
 *       因为四种形态的判据共享"这次按下"这个概念，分开存反而要再存一份形态。
 */
struct Slot {
  bool held = false;                 ///< 上一帧这组键是不是全部按下。
  bool fired = false;                ///< 这次按下已经出过动作（K-08 的闸门）。
  uint64_t press_start_ms = 0;       ///< 本次全部按下的起始时刻。
  uint64_t last_press_start_ms = 0;  ///< 上一次全部按下的起始时刻（双击量的就是这两次的间隔）。
  bool awaiting_second = false;      ///< 已经松过一次，正在等第二下。
  uint64_t pulse_until_ms = 0;       ///< 宏脉冲的结束时刻。
  bool pulsing = false;              ///< 宏脉冲正在进行。
};

/// 一个连点键的跨帧状态：当前半周期的相位与它的起始时刻。
struct TurboSlot {
  /// 缺省"开着"：连点只在键被按下时塑形，而一次按下的第一帧该是按下而不是空拍。
  bool on = true;
  uint64_t edge_ms = 0;   ///< 当前这个半周期是从哪一毫秒开始的（不是上一帧的时刻）。
};

/**
 * @brief 插件唯一的跨帧可变状态。
 * @note 只在 bt 的读线程上写（`on_state_in`），主循环一个字节都不碰；容量在
 *       `Reset` 里按规则条数定死，运行期不重分配。
 * @note touch 那一份只是**存**在这里，判据在 ids::TouchpadStates：和摇杆方向一样，
 *       "从模拟量或坐标判出位"是布局那一层的活，跨帧状态由这一层代管。
 */
struct State {
  ids::Mask direction_latch = 0;   ///< 上一帧判出的摇杆方向位（I-01 的迟滞就靠它）。
  ids::TouchState touch;           ///< 触摸板那一族：半区锁存、划动起点与事件位。
  std::vector<Slot> slots;         ///< 前段是 shortcuts，后段是 macros。
  std::vector<TurboSlot> turbos;
};

/**
 * @brief 按规则表定容并清空状态。
 * @param io 要复位的状态。
 * @param rules 刚解析好的规则表：槽数由它决定。
 * @details init 与"不武装之后再武装"都调这里。复位必须是全清：留着上一轮的
 *          `fired` 会让新配置里同一条规则的第一次按下被吞掉。
 */
void Reset(State& io, const rules::Set& rules);

/**
 * @brief 判定这一帧。
 * @param rules 只读的规则表。
 * @param frame 这次钩子收到的那份状态体的快照：判据读的是它，所以映射与连点的
 *              结果不会回头影响判定（K-07 由"拿不到可写的字节"保证）。
 * @param now_ms 这一帧的时刻，由调用方取。
 * @param io 跨帧状态，读写。
 * @return 这一帧的判定结果。
 * @note 纯位运算加一遍规则列表，运行期零分配；在 bt 的读线程上按帧调用，不许
 *       阻塞、不许打日志。
 */
FrameDecision Evaluate(const rules::Set& rules, const ids::Frame& frame,
                       uint64_t now_ms, State& io);

}  // namespace sense
}  // namespace buttons
