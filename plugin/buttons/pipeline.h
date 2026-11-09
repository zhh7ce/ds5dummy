/**
 * @file
 * @brief 一帧的执行：把判定结果与规则表作用在快照上，得到主机该看到的那 63 字节
 *
 * pipeline 不含时间、不持状态：同一份输入永远给同一个输出，所以表算法（链式
 * 不叠加、多对一 OR、按轴合并、同帧原始值）可以脱离插件的静态量单独测。
 */

#pragma once

#include "ids.h"
#include "rules.h"
#include "sense.h"

namespace buttons {
namespace pipeline {

/**
 * @brief 把规则作用到一份状态体上。
 * @param rules 只读的规则表。
 * @param decision 这一帧的判定结果：映射层要看的位（已经含摇杆方向与连点塑形）、
 *                  宏要强行按下的位、被强制取值的轴。
 * @param frame 这次钩子收到的那份状态体的快照：本函数取的所有值都来自它，
 *              不回读刚写进结果的字节的值（M-15 的"同一帧的原始值"因此是
 *              结构性的，不是靠纪律）。
 * @param state 63 字节状态体（kPluginStateSize）；进来时是手柄报的原始字节，
 *              返回时是主机将要读到的结果。写它只有末尾 EncodeFrame 这一处。
 * @note 纯函数，运行期零分配；在 bt 的读线程上按帧调用。
 */
void Apply(const rules::Set& rules, const sense::FrameDecision& decision,
           const ids::Frame& frame, uint8_t* state);

}  // namespace pipeline
}  // namespace buttons
