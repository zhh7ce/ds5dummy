/**
 * @file
 * @brief 触发判定的状态机：四种时序、连点相位、宏脉冲、摇杆方向命中期间的居中
 *
 * 这里集中了插件全部跨帧的判据。每个 slot 只知道自己那一组键的按下历史，规则
 * 之间不互相看——所以"一条规则的命中会不会抢下另一条的输入"这种问题在这一层
 * 根本不存在，也不需要有。
 */

#include "sense.h"

#include <cstddef>

namespace buttons {
namespace sense {
namespace {

using rules::Timing;

/**
 * @brief 这组键是不是全部按下。
 * @param terms 触发要求的那些源。
 * @param digits 这一帧的物理位（含判出来的摇杆方向位）。
 * @return 全部按下时 true；terms 为空永远 false（一条没有源的触发不可能命中）。
 */
bool AllDown(ids::Mask terms, ids::Mask digits) {
  return terms != 0 && (terms & digits) == terms;
}

/**
 * @brief 这组键是不是一个都没按着。
 * @param terms 触发要求的那些源。
 * @param digits 这一帧的物理位。
 * @return 一个都没按下时 true。
 * @details K-08 要求"这组键必须全部松开才允许再次触发"，闸门只在这里重新打开。
 *          以"不再全部按下"为复位点的话，按住组合里的一个键反复敲另一个键就会
 *          一下一下地出动作，那不是这句话的意思。
 */
bool AllReleased(ids::Mask terms, ids::Mask digits) {
  return (terms & digits) == 0;
}

/**
 * @brief 一个连点键的半周期长度。
 * @param hz 配置里的每秒次数。
 * @return 半个周期的毫秒数，至少 1。
 * @details 一帧只能表达一次开或关，所以一个完整周期至少两帧：这就是 T-02 那个
 *          "上限是帧率的一半"的来处，达成与否由相位推进决定，不在这里判。
 */
uint64_t HalfPeriodMs(uint16_t hz) {
  const uint64_t period = hz;   // 先升到无符号：下面整条算式都在毫秒域里
  const uint64_t half = (500 + period / 2) / period;   // 500/hz 四舍五入
  return half == 0 ? 1 : half;
}

/**
 * @brief 推进一条触发的状态机。
 * @param trigger 那条触发的时序形态。
 * @param physical 这一帧的物理位（映射与连点都还没参与）。
 * @param now_ms 这一帧的时刻。
 * @param slot 这条触发的跨帧状态，读写。
 * @return 这一帧命中了吗。
 * @details 两个沿各自负责什么：
 *          - 按下沿（这组键刚刚全部按下）：长按记下起始时刻；双击在这里数第二下。
 *          - 松手沿（这组键刚刚不再全部按下）：单击在这里结算——按住多久只有松手
 *            才知道（K-02、K-05）；双击在这里记下"可以算第二下了"。
 *          组合与长按不需要沿：它们要的就是"当前全部按下"这一件事，配上闸门
 *          就只会出一轮。
 */
bool Advance(const rules::Trigger& trigger, ids::Mask physical, uint64_t now_ms,
             Slot& slot) {
  const bool down = AllDown(trigger.terms, physical);
  bool fired = false;

  if (!down) {
    const bool broke = slot.held;   // 刚刚从"全部按下"落下来
    slot.held = false;
    if (broke) {
      if (trigger.timing == Timing::kTap && !slot.fired) {
        // 按住多久只有松手这一刻才知道（K-02）；起点是那组键全部按下的时刻。
        fired = now_ms - slot.press_start_ms < trigger.threshold_ms;
      }
      // 刚出过动作的那一轮不再等第二下：否则命中之后的第一次松手就重新武装，
      // 之后每一下都算一次双击，而一次双击要消耗两下。
      if (trigger.timing == Timing::kDouble && !slot.fired) {
        slot.awaiting_second = true;
      }
    }
    if (fired) slot.fired = true;
    if (AllReleased(trigger.terms, physical)) slot.fired = false;   // K-08
    return fired;
  }

  if (!slot.held) {
    slot.held = true;
    slot.press_start_ms = now_ms;
    if (trigger.timing == Timing::kDouble) {
      if (slot.awaiting_second &&
          now_ms - slot.last_press_start_ms <= trigger.threshold_ms) {
        fired = true;
      }
      slot.awaiting_second = false;
      slot.last_press_start_ms = now_ms;
    }
  }

  if (!slot.fired && !fired) {
    switch (trigger.timing) {
      case Timing::kChord: fired = true; break;
      case Timing::kLong:
        fired = now_ms - slot.press_start_ms >= trigger.threshold_ms;
        break;
      case Timing::kTap:
      case Timing::kDouble: break;   // 上面两个沿里已经判过了
    }
  }
  if (fired) slot.fired = true;
  return fired;
}

/**
 * @brief 哪些源被某条规则当作源用过。
 * @param rules 规则表。
 * @return 源位集合。
 * @details I-02 的"摇杆作为源时把摇杆本身居中"要的是"这个方向确实被绑过"：没有
 *          任何规则用它的时候，静置摇杆不该被插件动过一根轴。
 */
ids::Mask BoundSources(const rules::Set& rules) {
  ids::Mask bound = 0;
  for (const rules::Remap& rule : rules.remaps) bound |= ids::BitOf(rule.src);
  for (const rules::Turbo& turbo : rules.turbos) bound |= ids::BitOf(turbo.src);
  for (const rules::Shortcut& shortcut : rules.shortcuts) {
    bound |= shortcut.trigger.terms;
  }
  for (const rules::Macro& macro : rules.macros) bound |= macro.trigger.terms;
  return bound;
}

/**
 * @brief 把一根摇杆的两根轴都设成强制居中。
 * @param axis 那根摇杆的任意一根轴（方向位只告诉我们是哪一根）。
 * @param decision 本帧的判定结果。
 */
void CenterStick(ids::Axis axis, FrameDecision& decision) {
  for (const ids::Axis entry : {axis, ids::StickOther(axis)}) {
    const std::size_t index = ids::IndexOf(entry);
    decision.forced_axis[index] = ids::kStickCenter;
    decision.forces_axis[index] = true;
  }
}

/**
 * @brief 宏的目标位里那两个扳机的轴那一半按全按给出（A-03）。
 * @param digits 宏的目标位集合。
 * @param decision 本帧的判定结果。
 */
void PressTriggers(ids::Mask digits, FrameDecision& decision) {
  for (const ids::Source src : {ids::Source::kL2, ids::Source::kR2}) {
    if ((digits & ids::BitOf(src)) == 0) continue;
    const std::size_t index = ids::IndexOf(ids::TriggerAxisOf(src));
    decision.forced_axis[index] = ids::kTriggerPressed;
    decision.forces_axis[index] = true;
  }
}

}  // namespace

void Reset(State& io, const rules::Set& rules) {
  io.direction_latch = 0;
  io.touch = {};   // 半区与划动起点都留着的话，一次新的接触会被算进上一轮的手势。
  io.slots.assign(rules.shortcuts.size() + rules.macros.size(), Slot{});
  io.turbos.assign(rules.turbos.size(), TurboSlot{});
}

FrameDecision Evaluate(const rules::Set& rules, const ids::Frame& frame,
                       uint64_t now_ms, State& io) {
  FrameDecision decision;

  // 1. 判出来的位。摇杆方向读快照里那四根轴的原始值，所以轴互换与轴反向改不到
  //    它（M-17）；触摸板那族读那两个触点，半区与划动起点锁存在 io.touch 里。
  io.direction_latch =
      ids::StickDirections(frame, ids::kStickStep, ids::kStickHysteresis,
                           io.direction_latch);
  const ids::Mask physical =
      frame.digits | io.direction_latch | ids::TouchpadStates(frame, io.touch);
  decision.digits = physical;

  // 2. 触发判定：读的是塑形之前的 physical，映射与连点的结果不会回头影响它（K-07）。
  std::size_t slot_index = 0;
  for (const rules::Shortcut& shortcut : rules.shortcuts) {
    if (Advance(shortcut.trigger, physical, now_ms, io.slots[slot_index])) {
      if (shortcut.action == rules::Action::kPowerOff) decision.power_off = true;
    }
    ++slot_index;
  }
  for (const rules::Macro& macro : rules.macros) {
    Slot& slot = io.slots[slot_index++];
    if (Advance(macro.trigger, physical, now_ms, slot)) {
      slot.pulsing = true;
      slot.pulse_until_ms = now_ms + rules::kDefaultPulseMs;
    }
    // 脉冲是一次性的（A-05）：到点就收，不看那组键还按不按着。
    if (!slot.pulsing) continue;
    if (now_ms >= slot.pulse_until_ms) {
      slot.pulsing = false;
      continue;
    }
    decision.inject |= macro.digits;
    PressTriggers(macro.digits, decision);
  }

  // 3. 连点：每跨过一个半周期翻一沿，帧率跟不上时少翻（T-02 的"宁可偏低"）。
  for (std::size_t i = 0; i < rules.turbos.size(); ++i) {
    const rules::Turbo& turbo = rules.turbos[i];
    TurboSlot& phase = io.turbos[i];
    const ids::Mask bit = ids::BitOf(turbo.src);
    if ((physical & bit) == 0) {
      // 松手立即停（T-01），并且下一次按下从"开着"这一半开始：从关起算的话，
      // 第一帧读到的是一次空拍，玩家那一下按下去游戏看不见。
      phase.on = true;
      phase.edge_ms = now_ms;
      continue;
    }
    const uint64_t half = HalfPeriodMs(turbo.hz);
    const uint64_t elapsed = now_ms - phase.edge_ms;
    if (elapsed >= 2 * half) {
      // 落后超过一个完整周期：从现在重新起算，一个沿都不补。逐帧追欠的话，欠得
      // 越久之后每一帧都要翻一次，那是一串真机上根本不该出现的快闪；而配置刚
      // 下来的第一帧本来就没有"上一个沿"可追。
      phase.edge_ms = now_ms;
    } else if (elapsed >= half) {
      phase.on = !phase.on;
      phase.edge_ms += half;
    }
    if (phase.on) decision.digits |= bit;
    else decision.digits &= ~bit;
  }

  // 4. 命中期间把摇杆居中（I-02 的缺省那一半；always 那一半在 rules 的轴表里）。
  //    遍历只走那八个方向：触摸板那一族同样是判出来的位，但它没有"自己的摇杆"，
  //    StickAxisOf 对它返回 Axis::kCount，走进 CenterStick 就是往一根不存在的轴上
  //    写强制值。
  const ids::Mask bound = BoundSources(rules);
  for (uint8_t i = static_cast<uint8_t>(ids::Source::kLstickUp);
       ids::IsStickDirection(static_cast<ids::Source>(i)); ++i) {
    const ids::Source src = static_cast<ids::Source>(i);
    const ids::Mask bit = ids::BitOf(src);
    if ((io.direction_latch & bit) == 0 || (bound & bit) == 0) continue;
    CenterStick(ids::StickAxisOf(src), decision);
  }

  return decision;
}

}  // namespace sense
}  // namespace buttons
