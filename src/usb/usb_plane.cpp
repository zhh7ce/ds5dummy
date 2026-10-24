/**
 * @file usb_plane.cpp
 * @brief 数据面共享状态的唯一实例
 *
 * 只有 `Get()` 那一个进程级 static；全部字段的含义与并发契约见
 * usb_plane.h，循环本体不在这里（usb_inout.cpp、usb_ep0.cpp）。
 */

#include "usb/usb_plane.h"

namespace usb::plane {

DataPlane& Get() {
  static DataPlane plane;
  return plane;
}

}  // namespace usb::plane
