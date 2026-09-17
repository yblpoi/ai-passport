<p align="right">
  <strong>简体中文</strong> · <a href="serial-screenshot.md">English</a>
</p>

# 经串口控制台截取设备屏幕

改排版本来必须有人盯着那块 240×320 的屏。`shot`(以及它的协议别名
`FAP_SCREENSHOT_V1`)把当前屏幕经 USB 串口控制台直接吐出来,`key` 再注入一次
按键事件 —— 于是一屏一屏都能自动走到、自动抓下来。

## 协议

主机发一行 ASCII;设备回一行头,再紧跟恰好那么多字节的小端 RGB565 像素:

```text
> FAP_SCREENSHOT_V1
FAP_SCREENSHOT_V1 240 320 RGB565LE 153600
<153600 字节>
```

`tools/screenshot.py` 会说这套协议,并写成 PNG:

```bash
python3 tools/screenshot.py                 # 240x320 -> /tmp/screen.png
python3 tools/screenshot.py -o list.png     # 指定输出
python3 tools/screenshot.py --raw dump.bin  # 另外留一份原始像素
```

除 `pyserial` 外不需要任何第三方库(PNG 用 `zlib` 现写)。协议本身与
`docs/reference/y2lin/serial-screenshot-protocol.md` 描述的是同一套,那份文档
是另一个应用对它的记录,踩坑清单值得一读 —— 但它的核心做法在这里用不了:

## 为什么本实现不预留整屏缓冲

那份实现静态预留了 240×320×2 = 150 KB 的整屏缓冲,再用
`lv_snapshot_take_to_draw_buf()` 渲染进去。它跑在一个空闲堆约 220 KB 的固件上;
而本固件总共只有约 204 KB RAM、开着蓝牙时只剩 27 KB —— 静态占掉 150 KB 根本
启动不起来。

本实现在 `main/love_shot.c` 里改挂 `LV_EVENT_FLUSH_START`(BSP 已经在用同一个
事件做圆角遮罩),把每次刷新的分块**按顺序直接流出去**。本板 LVGL 绘制缓冲是
240×20,整屏重绘正好切成 16 条整宽的带,拼起来就是协议承诺的那幅图。这条前提
**逐块校验**:一旦某块不是整宽、或者起点接不上上一块的末尾,本次传输立刻中止,
宁可让主机报错也不发一张错位的图。

## 会咬人的地方

- **渲染必须发生在 LVGL 任务上,不能在控制台任务里。** 整屏软件渲染要 ~7KB 栈
  (LVGL 端口自己的任务就是 7168),而控制台任务只有 4096 —— 在那里渲染会直接
  栈保护复位。所以命令用 `lv_async_call()` 把抓图投给 LVGL 任务,自己在信号量上等。
- **二进制窗口期间必须静默所有日志。** 主机是按声明字节数精确读取的,窗口里混进
  一个日志字节整幅图就错位。命令在发出第一个头行字节前
  `esp_log_level_set("*", ESP_LOG_NONE)`,最后一个像素字节之后恢复原级别。
- **分块要小于发送环形缓冲。** 控制台 REPL 装驱动时用的是默认 256 字节缓冲,
  `usb_serial_jtag_write_bytes()` 对超过容量的整块写入会立刻失败。这里按 128 字节发。
- **驱动必须先装好。** 没装驱动就去读那个口会解引用空的驱动对象,设备反复重启
  (现象是屏幕一直在闪)。`love_console_start()` 会装;抓图前会先确认它在。
- **一整份 `love_config_t` 已经放不进这些任务的栈了。** 配置 v4 起它是 1454 字节
  (24 条事件),放在局部变量里把控制台任务与 HTTP 任务的栈都顶穿过。那两条路径改成
  走堆。`status` 命令会打控制台、网页、界面、蓝牙四个任务的剩余栈 —— 当初就是靠
  它发现的,以后加字段也照这个办法核。

## 从控制台驱动界面

`key up|down|ok|long` 会走一遍 `love_app_key()`,和真的按一下完全等价。配上
`shot`,每一屏都能从脚本里验:

```bash
python3 tools/screenshot.py -o main.png
printf 'key down\n' | python3 -c '...'      # 或者直接用串口终端敲
python3 tools/screenshot.py -o list.png
```

注意:设备要插着 USB(像素走串口,蓝牙通知那条路太慢);打开串口时驱动会把开机
时滞留在环形缓冲里的日志回放出来,先读掉再信后面的输出;`key` 是在控制台任务上渲染
的,余量不大 —— 真实按键走的是 esp_timer 任务(3584 字节),那才是常规路径。
