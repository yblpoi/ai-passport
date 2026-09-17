#!/usr/bin/env python3
"""从 USB 串口抓一张设备屏幕,存成 PNG。

协议(FAP_SCREENSHOT_V1,见 docs/reference/y2lin/serial-screenshot-protocol.md):
主机发一行命令,设备回一行
    FAP_SCREENSHOT_V1 <宽> <高> RGB565LE <字节数>
再紧跟恰好这么多字节的小端 RGB565 像素。二进制期间设备会把日志全部静默。

用法:
    python3 tools/screenshot.py                     # 找一个 USB 串口,存到 /tmp/screen.png
    python3 tools/screenshot.py -o list.png         # 指定输出
    python3 tools/screenshot.py -p /dev/cu.usbmodem1101 -c shot
    python3 tools/screenshot.py --raw dump.bin      # 同时留一份原始像素

逐屏核对一整套界面(主页/单页卡/列表各页)用 --keys:它在**同一次串口会话**里
先注入按键再抓图,输出 00-start.png、01-down.png 这样的一串。-o 给目录。

    python3 tools/screenshot.py --keys down,down,down -o /tmp/shots

为什么非要在同一次会话里:打开串口会复位芯片(USB-serial-JTAG 的行为),而界面停在
哪一屏是内存里的状态 —— 一屏一个脚本跑下来,每次都会被复位回主屏。

不需要任何第三方库:PNG 用 zlib + 手写块写出来。
"""

from __future__ import annotations

import argparse
import glob
import os
import struct
import sys
import time
import zlib

try:
    import serial
except ImportError:  # pragma: no cover - 只在缺少依赖时走到
    print("需要 pyserial:pip install pyserial", file=sys.stderr)
    raise SystemExit(2)

HEADER_TAG = b"FAP_SCREENSHOT_V1 "
DEFAULT_COMMAND = "shot"


def find_port() -> str:
    """挑一个 USB 串口。macOS 上是 /dev/cu.usbmodem*,Linux 上是 ttyACM*/ttyUSB*。"""
    for pattern in ("/dev/cu.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*"):
        found = sorted(glob.glob(pattern))
        if found:
            return found[0]
    raise SystemExit("没找到 USB 串口;设备插好了吗?")


def write_png(path: str, width: int, height: int, pixels: bytes) -> None:
    """把 RGB888 像素写成 PNG(每行 filter 0,不压缩优化,够用)。"""
    stride = width * 3
    raw = bytearray()
    for y in range(height):
        raw.append(0)                                   # filter type
        raw += pixels[y * stride:(y + 1) * stride]

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)   # 8bit truecolor
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", header))
        fh.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        fh.write(chunk(b"IEND", b""))


def rgb565le_to_rgb888(data: bytes) -> bytes:
    out = bytearray(len(data) // 2 * 3)
    j = 0
    for i in range(0, len(data) - 1, 2):
        value = data[i] | (data[i + 1] << 8)
        r = (value >> 11) & 0x1F
        g = (value >> 5) & 0x3F
        b = value & 0x1F
        out[j] = (r << 3) | (r >> 2)
        out[j + 1] = (g << 2) | (g >> 4)
        out[j + 2] = (b << 3) | (b >> 2)
        j += 3
    return bytes(out)


def read_exactly(ser, count: int, deadline: float) -> bytes:
    buf = bytearray()
    while len(buf) < count:
        chunk = ser.read(count - len(buf))
        if chunk:
            buf += chunk
            continue
        if time.time() > deadline:
            break
    return bytes(buf)


def capture(ser, command: str = DEFAULT_COMMAND, timeout: float = 20.0):
    """在同一条已打开的串口上抓一张图。返回 (width, height, 原始 RGB565 字节)。"""
    ser.reset_input_buffer()
    ser.write((command + "\n").encode())
    ser.flush()

    # 找头行。命令本身会被回显,所以要一直扫到真的头行为止。
    window = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = ser.read(256)
        if not chunk:
            continue
        window += chunk
        index = window.find(HEADER_TAG)
        if index < 0:
            # 头行可能被拆在两个读之间,留够一个头行的长度。
            if len(window) > 256:
                del window[:-64]
            continue
        end = window.find(b"\n", index)
        if end < 0:
            continue
        text = window[index:end].decode("ascii", "replace").strip()
        parts = text.split()
        # FAP_SCREENSHOT_V1 <w> <h> RGB565LE <n>
        if len(parts) != 5 or parts[0] != "FAP_SCREENSHOT_V1":
            del window[:index + len(HEADER_TAG)]
            continue
        width, height, fmt, size = int(parts[1]), int(parts[2]), parts[3], int(parts[4])
        if fmt != "RGB565LE":
            raise SystemExit(f"不认识的像素格式: {fmt}")
        # 头行之后可能已经顺手读到了一些像素,别丢。
        leftover = bytes(window[end + 1:])
        data = leftover + read_exactly(ser, size - len(leftover), time.time() + timeout)
        if len(data) != size:
            raise SystemExit(f"数据在声明长度之前就结束:{len(data)}/{size} 字节")
        return width, height, data

    raise SystemExit(f"没有收到截图头行(超时 {timeout}s)。设备在跑吗?命令对了吗?")


def wait_prompt(ser, timeout: float = 4.0) -> bytes:
    """等到控制台提示符(或超时),顺便把读到的字节交出去。"""
    buf = bytearray()
    end = time.time() + timeout
    while time.time() < end:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            if buf.rstrip().endswith(b">"):
                break
    return bytes(buf)


def run_key_sequence(port: str, keys: list[str], out_dir: str, timeout: float) -> int:
    """同一次会话里注入一串按键,每步抓一张图。"""
    os.makedirs(out_dir, exist_ok=True)
    with serial.Serial(port, 115200, timeout=0.2) as ser:
        # 开串口会复位芯片:先等它起来、把启动日志读掉,再开始注入。
        time.sleep(2.0)
        wait_prompt(ser, 6.0)

        steps = [("start", None)] + [(key, key) for key in keys]
        for i, (name, key) in enumerate(steps):
            if key:
                ser.write(f"key {key}\n".encode())
                ser.flush()
                if not wait_prompt(ser):
                    print(f"key {key}: 无响应(可能丢了一条)", file=sys.stderr)
            width, height, data = capture(ser, timeout=timeout)
            path = os.path.join(out_dir, f"{i:02d}-{name}.png")
            write_png(path, width, height, rgb565le_to_rgb888(data))
            print(f"key {key or '(未按键)'}: {width}x{height} -> {path}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="从 USB 串口抓一张设备屏幕")
    parser.add_argument("-p", "--port", default=None, help="串口设备;省略则自动挑一个")
    parser.add_argument("-o", "--out", default="/tmp/screen.png",
                        help="输出 PNG 路径;带 --keys 时是输出目录")
    parser.add_argument("-c", "--command", default=DEFAULT_COMMAND, help="触发的控制台命令")
    parser.add_argument("--raw", default=None, help="额外保存一份原始 RGB565 像素")
    parser.add_argument("--keys", default=None,
                        help="同一次会话里依次注入按键(逗号分隔:down,down,ok,long),每步抓一张")
    parser.add_argument("--timeout", type=float, default=20.0, help="整幅图的等待上限(秒)")
    args = parser.parse_args()

    port = args.port or find_port()

    if args.keys is not None:
        keys = [k for k in args.keys.split(",") if k]
        return run_key_sequence(port, keys, args.out, args.timeout)

    with serial.Serial(port, 115200, timeout=0.2) as ser:
        width, height, data = capture(ser, args.command, args.timeout)

    if args.raw:
        with open(args.raw, "wb") as fh:
            fh.write(data)

    write_png(args.out, width, height, rgb565le_to_rgb888(data))
    print(f"{width}x{height} {len(data)} 字节 -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
