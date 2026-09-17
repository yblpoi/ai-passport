#!/usr/bin/env python3
"""电脑端蓝牙串口终端:像串口一样连到设备上敲命令。

设备广播一个标准 Nordic UART Service(见 docs/development/engineering/wifi-provisioning.md),
所以电脑只要有一块蓝牙网卡就能调参——不必连 Wi-Fi、不必开热点、不必掏手机。
命令集与 USB 串口控制台完全相同(help / status / time / wifi / ble)。

用法(仓库根目录):
    pip install bleak
    python3 tools/ble_console.py                 # 自动选第一个 LoveCount-*
    python3 tools/ble_console.py LoveCount-8C5E  # 指定广播名
    python3 tools/ble_console.py AA:BB:CC:DD:EE:FF   # 指定地址
    printf 'status\\n' | python3 tools/ble_console.py   # 非交互:喂一批命令

前置条件:设备端蓝牙要是开着的(USB 串口或设置页里 `ble on`)。设备出厂默认关,
而且**没人连接满 5 分钟会自动关**——搜不到设备时先确认这两点。

macOS 首次运行会弹蓝牙权限,要点允许;Linux 需要 bluetooth 服务在跑且当前用户在
bluetooth 组里。
"""

from __future__ import annotations

import argparse
import asyncio
import sys

from bleak import BleakClient, BleakScanner
from bleak.uuids import normalize_uuid_str

# 标准 NUS UUID:6E400001 服务 / 6E400002 主机→设备 / 6E400003 设备→主机。
NUS_RX = normalize_uuid_str("6e400002-b5a3-f393-e0a9-e50e24dcca9e")
NUS_TX = normalize_uuid_str("6e400003-b5a3-f393-e0a9-e50e24dcca9e")

# 广播名前缀,与 love_ble_device_name() 生成的一致。
NAME_PREFIX = "LoveCount-"

# 管道模式下命令发完后,再等这么久收输出(命令是异步排队的,发完就退会丢掉回显)。
DRAIN_SECONDS = 3.0


async def pick_device(target: str | None):
    """按广播名或地址找设备;target 为空时取第一个 LoveCount-*。"""
    if target and target.count(":") == 5:
        return target   # 已经是 MAC 地址,交给 bleak 直接连

    print("扫描中…")
    found = await BleakScanner.discover(timeout=8.0, return_adv=True)
    chosen = None
    for _, (device, adv) in found.items():
        label = adv.local_name or ""
        if not label.startswith(NAME_PREFIX):
            continue
        print(f"  {label:20s} {device.address}  rssi={adv.rssi}")
        if chosen is None and target in (None, label):
            chosen = device
    return chosen


async def pump_stdin(client: BleakClient, stop: asyncio.Event) -> None:
    """把 stdin 的每一行写进 RX 特征。stdin 读完(管道)就排空后退出。"""
    loop = asyncio.get_running_loop()
    queue: asyncio.Queue = asyncio.Queue()

    def reader() -> None:
        for line in sys.stdin:
            loop.call_soon_threadsafe(queue.put_nowait, line.rstrip("\n"))
        loop.call_soon_threadsafe(queue.put_nowait, None)   # EOF

    loop.run_in_executor(None, reader)

    while not stop.is_set():
        try:
            line = await asyncio.wait_for(queue.get(), timeout=0.3)
        except asyncio.TimeoutError:
            continue
        if line is None:
            await asyncio.sleep(DRAIN_SECONDS)
            return
        if line == "":
            continue
        # 行尾用 \r\n:BLE 串口的通行约定,设备端 \n / \r\n / \r 都认。
        await client.write_gatt_char(NUS_RX, (line + "\r\n").encode(), response=False)


async def main() -> int:
    parser = argparse.ArgumentParser(description="电脑端蓝牙串口终端")
    parser.add_argument("target", nargs="?", default=None,
                        help="广播名(如 LoveCount-8C5E)或地址;省略则取第一个 LoveCount-*")
    args = parser.parse_args()

    device = await pick_device(args.target)
    if device is None:
        print("没找到设备。确认蓝牙已打开(`ble on`)、没被别的手机连着、且在范围内。")
        return 1

    stop = asyncio.Event()

    def on_notify(_char, data: bytearray) -> None:
        # 设备回的是 UTF-8 文本,原样打到终端(不分行缓冲,半行也要立刻可见)。
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()

    def on_disconnect(_client) -> None:
        print("\n[已断开]")
        stop.set()

    async with BleakClient(device, disconnected_callback=on_disconnect) as client:
        print(f"已连接 {device}")
        # 订阅之后设备会主动推一份 help,所以命令行里会先出现一份用法。
        await client.start_notify(NUS_TX, on_notify)
        print("订阅完成。命令:help / status / time <Unix秒> / wifi / ble off;Ctrl-C 退出。\n")
        await pump_stdin(client, stop)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        print("\n已退出。")
