// main/love_shot.h —— 串口截屏(FAP_SCREENSHOT_V1)。
//
// 主机经 USB 串口发一行命令,设备回一行头
//   FAP_SCREENSHOT_V1 <宽> <高> RGB565LE <字节数>
// 再紧跟恰好这么多字节的小端 RGB565 像素。协议与踩过的坑见
// docs/reference/y2lin/serial-screenshot-protocol.md。
//
// 与本仓库那篇记录的关键差别:**那块整屏静态缓冲我们留不起**。那份实现跑在一个
// 空闲堆约 220KB 的固件上,而本固件总可用 RAM 才 ~204KB、BLE 开着时只剩 27KB,
// 静态预留 240×320×2 = 150KB 会直接把固件压死。这里改用另一种不需要整屏缓冲的
// 办法:截获 LVGL 的 LV_EVENT_FLUSH_START(BSP 已经在用它做圆角遮罩),把每次
// 刷新的分块**按顺序直接流出去**。本板的绘制缓冲是 240×20,整屏重绘正好切成
// 16 条自顶向下的整宽带,拼起来就是整幅图 —— 但这条前提会被逐块校验,一旦
// 分块不是"整宽且连续",本次传输立刻中止,宁可让主机报错也不发一张错位的图。
#pragma once

#include <stdbool.h>

// 抓一屏并发给主机。返回 true 表示整幅图已完整发出。
// 只走 USB 串口(蓝牙那条链路带宽不够,也拿不到二进制流)。
bool love_shot_send(void);
