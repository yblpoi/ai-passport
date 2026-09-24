<p align="right">
  <strong>简体中文</strong> · <a href="wifi-provisioning.md">English</a>
</p>

# 通过蓝牙配网连接 Wi-Fi

当用户需要设备通过 Wi-Fi 连接网络时，可以参考
[`demo/blufi-provisioning` 分支](https://github.com/FoloToy/ai-passport/tree/demo/blufi-provisioning)
的代码实现蓝牙配网。手机通过 BLE 上的 BLUFI 协议发送 Wi-Fi 名称（SSID）和
密码，设备以 Wi-Fi STA 模式连接网络并回报连接状态。蓝牙用于传递配网信息，
不是承载应用的互联网流量。

这是可选的应用参考。当前 `main` 的 Wi-Fi demo 仅扫描网络，并未实现联网或
蓝牙配网；没有联网需求的应用不必启用网络功能。

<a id="mini-program-name"></a>

## 小程序名称

配套小程序的准确名称为：**蓝牙配网-FoloToy AI PASSPORT**。

向用户说明或搜索小程序时，请使用上述完整名称，不要自行翻译或改写。
这是小程序名称，不是设备的蓝牙广播名；参考固件的广播名为
`BLUFI_FoloPassport`。

预期流程：设备进入配网状态，手机打开蓝牙并授予所需权限，打开该小程序，
选择目标设备，提供 2.4 GHz Wi-Fi 的名称和密码。确认设备取得 IP 地址后，
再验证应用实际需要的网络请求；仅蓝牙连接成功不能证明能够访问互联网。

## 代码参考入口

- [`main/demo_blufi.c`](https://github.com/FoloToy/ai-passport/blob/demo/blufi-provisioning/main/demo_blufi.c)：BLUFI 回调、Wi-Fi 扫描与连接、凭据处理、状态回报和生命周期。
- [`main/demo_blufi_security.c`](https://github.com/FoloToy/ai-passport/blob/demo/blufi-provisioning/main/demo_blufi_security.c) 及其头文件：BLUFI 安全协商回调，提取示例时不要丢弃这些逻辑。
- [`main/demo_radio.c`](https://github.com/FoloToy/ai-passport/blob/demo/blufi-provisioning/main/demo_radio.c)：NVS、`esp_netif` 和默认事件循环的共享初始化。
- [`main/CMakeLists.txt`](https://github.com/FoloToy/ai-passport/blob/demo/blufi-provisioning/main/CMakeLists.txt) 和 [`sdkconfig.defaults`](https://github.com/FoloToy/ai-passport/blob/demo/blufi-provisioning/sdkconfig.defaults)：源文件注册、依赖、NimBLE/BLUFI 和加密配置。不能只复制单个 C 文件就认为完成接入。

## 接入与验收

从应用当前基线出发，记录参考提交，仅移植适用的联网逻辑。不要直接合并整个
demo，也不要用该分支的旧版 BSP、分区表或配置覆盖当前版本。重新实现应用
自己的界面，将配网状态、任务及 Wi-Fi/BLE 服务放在应用层。

明确配网入口与退出、超时、重试上限、凭据持久化及主动清除凭据的方式。
禁止记录或提交 Wi-Fi 密码。保留 LVGL 锁、非阻塞回调、任务与事件处理器清理
机制，并评估 Wi-Fi、BLE 和 UI 同时运行的内部 RAM 占用。示例不等于完整的
生产环境授权或安全设计。

运行[验证门禁](build-and-test.zh_CN.md)，再遵循
[真机测试交接流程](../ai-guide.zh_CN.md#主动询问真机测试)。使用上述小程序
验证：发现设备、成功配网、密码错误与网络不可用、重连与重启、清除凭据、
反复进入退出配网，以及应用实际需要的网络请求。编译成功不能证明小程序
兼容性或实机联网正常；未执行的检查列入 `Unverified`。

## 本应用的配网入口

本应用提供三条独立的凭据配置入口，其中后两条共用同一套命令。三者并存，互不替代。

| 入口 | 用法 | 说明 |
| --- | --- | --- |
| USB 串口控制台 | `wifi <名称> <密码>` | `main/love_console.c`，走 USB-Serial-JTAG 控制台。**唯一在设备完全不可达时仍然可用的入口**——而设备配错网络时恰好就是这种状态。 |
| 蓝牙串口控制台 | 在 BLE 串口 App 里敲 `wifi <名称> <密码>` | `main/love_ble.c` 广播一个标准 NUS 服务，手机装上现成的 BLE 串口 App 即可，命令与 USB 控制台完全一致。出厂默认关闭。 |
| 后台网页 | “网络与热点”卡片 | `main/love_httpd.c`，经设备自带热点或局域网地址访问。前提是这两者之一可达，见[热点生命周期](#热点生命周期)。 |

三条凭据路径都汇聚到 `main/love_net.c` 的 `love_net_set_credentials()`、
`love_net_forget_ssid()` 与 `love_net_forget()`，因此只有一条凭据通路、一份 NVS 记录。
控制台里不带参数的 `wifi` 打印当前状态，`wifi open <名称>` 保存开放网络，
`wifi list` 列出已保存的热点，`wifi del <序号|名称>` 删掉其中一个，
`wifi clear` 清除全部凭据并重新打开热点。

控制台**不打印密码、也不回读密码**：`love_store_load_wifi_list()` 是唯一的读取方，
且只被网络层调用。控制台按空格切分参数，因此名称或密码含空格时请改用后台网页。

### 记多个热点：怎么选、怎么退避

设备最多记住 **5 个**热点（`LOVE_WIFI_MAX`），按保存顺序排在 `love_net.c` 的
`s_saved[]` 里。开机与掉线后的选网顺序是：

1. **先扫描**（约 1~2 秒），在已保存的网络里挑信号最强的那个连；
2. 这个候选失败后，剩下的候选**这一轮扫到过的优先**（信号强的先）；一张都没扫到才
   按保存顺序走。保存顺序是用户心里的优先级，但一份列表可能横跨好几个地方，只按它
   顺序试，会把整轮时间花在已经搬走的网络上，而身边那张网要等下一轮。隐藏 SSID 永远
   不会出现在扫描结果里，这也是"按保存顺序"这条路必须留着的原因；
3. 每个候选 15 秒（`CONNECT_TIMEOUT_MS`），从它**自己开始的那一刻**算起 —— 这个比较
   刻意带符号，见下；
4. 一整轮都没连上就退避重来：30 秒起、每轮翻倍、上限 5 分钟。

那 15 秒是用 `love_net_pick_elapsed_ms()` 量的，**不许**改回裸的无符号相减：心跳在每次
迭代开头先取 `now`，而扫描结果会在同一次迭代里启动新候选，此时 `since` 比 `now` 新，
无符号相减下溢成一个巨大的正数，刚选出来的候选当场被当成"15 秒一个事件都没来"丢掉 ——
而且它的"已试"位已经置上。结果是扫描刚扫到的那张最强网络每一轮都在几毫秒内被丢弃，
一整轮都没被真正试过。

退避期间**不发起重连**，但每次尝试都会留下失败原因：每条候选失败打一行 `love_net`
（带 Wi-Fi 断开事件的 `reason` 码），候选 15 秒内一个事件都没来再打一行，整轮失败
另有一条汇总（节流常量见 `main/love_net.c` 的 `ROUND_LOG_MIN_INTERVAL_MS`，稳态下
就是 5 分钟一条）。这是刻意的：早先的实现在断开事件里立刻重连，配过网但热点不在时
会以每秒一次的频率刷屏，那既是日志噪音也是无谓的射频活动。要看比这更细的驱动层
线索，再把驱动日志打开：`log wifi info`。

改凭据的语义：同名 SSID 就地更新密码并保留原来的顺序，新名称追加在末尾，列表满了
先在网页或 `wifi del` 里删掉一个。改的是"当前这条"会断开重连，改别的热点不动现有连接
——加一个备用网络不该把正在用的那个踢掉。

### 热点生命周期

热点名是 `LoveCount-XXXX`：SSID 仍由 MAC 派生，而密码改为**首次启动时随机生成并存进
NVS**（`love_store_load_ap_pass()`，显示在设备屏幕上）。原先密码也由同一个 MAC 派生，
于是任何看到 SSID 的人都能算出密码，再进没有任何鉴权的后台页。它只在三种情况下**自动**打开：

| 触发 | 条件 | 代码 |
| --- | --- | --- |
| 开机 | 设备没有已保存的凭据 | `love_net_init()` |
| 深睡唤醒 | 睡前热点开着，且没有"手动关闭"的闸 | `love_net_deinit()` 记进 RTC 保留内存，`love_net_init()` 恢复 |
| 联网失败兜底 | 有凭据，但连续 60 秒既没连上、也不在选网过程中 | `love_net_poll()` |

深睡唤醒＝重启，所以"用户要这个热点"这个意图只能靠 RTC 保留内存带过那一觉：没有这份
快照时，配过网的设备醒来后热点是关着的，要等下面那条 60 秒兜底才回来；而一旦立了
"手动关闭"的闸，就连兜底也不会回来。闸永远优先于快照。

兜底计时只在"没在尝试连接、也没有扫描在飞"的空档里走：一轮候选要花掉几十秒，过程中
切到 APSTA 会把正在进行的连接打断（`esp_wifi_set_mode()` 会重启射频），那等于白试一轮；
同一次切换还会把正在进行的扫描作废，而这一轮也就退化成按保存顺序盲试。扫描只有
1~3 秒，等它落地再兜底很便宜。

它也可以按需打开：设备设置页的「后台热点」、后台网页的网络卡片，或控制台的
`ap on`。热点开着且设备已联网时，**连续 5 分钟没有任何活动**（网页每个请求、机身
每次按键都算活动）它会自行关闭；这次自动关闭**刻意不算**手动关闭，所以上面的
兜底逻辑之后仍然有效。

开与关都经过 `apply_mode()` —— 全工程只有它动驱动模式。它**报错而不是中止固件**：
设置页那一行是在 input 任务上跑的，可能正撞上选网扫描，而这里原来用 `ESP_ERROR_CHECK`，
一次切换被拒就把整台设备重启 —— 从外面看正是"按了热点那一行，什么都没发生"。
现在失败只记一条 error，并交给 1 秒心跳重试（`s_mode_dirty`），按下的那一下最迟一秒后
生效。每次按下都会留一行日志（`开热点` / `手动关热点`），这是把"按键根本没到网络层"
与"切换本身失败"分开的唯一依据。

切换模式会重启射频，所以开关热点的那一下已连上的站点会短暂掉线（日志里是
`与 "X" 的连接断开(原因 8)`），大约两秒后由一轮正常选网连回来。这是驱动的行为而不是
缺陷：`esp_wifi_set_mode()` 没法在不把站点一起停掉的前提下增删 AP 接口。

从设备、网页或 `ap off` 关掉热点会置上一道*手动关闭*的闸，记在同一个 NVS 命名空间
的 `ap_off` 键里。闸开着时上面两条自动路径都让路：热点不会再背着用户回来，重启或
深睡醒来也一样。`ap on` 与 `wifi clear` 会撤销这道闸（清凭据必须留下配网入口）。
闸开着、STA 又连不上时，回到网络只剩两条路：设备设置页与两个串口控制台。

### 蓝牙串口控制台

`main/love_ble.c` 把 USB 控制台那张命令表（`main/love_console.c`）原样搬到
**标准 Nordic UART Service (NUS)** 上，所以 Serial Bluetooth Terminal、nRF Connect
这类现成 App 不用手配 UUID 就能收发。设备广播名是 `LoveCount-XXXX`（`XXXX` 取自
蓝牙 MAC 后两字节）。

| 角色 | UUID |
| --- | --- |
| 服务 | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| 手机 → 设备（写入） | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |
| 设备 → 手机（通知） | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` |

128 位 UUID **刻意不放进广播包**：31 字节的广播包装不下 flags、设备名和一个
128 位 UUID，硬塞会让 `ble_gap_adv_set_fields()` 返回 `EMSGSIZE`。

可用命令：`help`、`status`（时间/网络/蓝牙/当前屏与页码/内存/各任务栈余/LVGL 池用量与事件显示序）、
`wifi …`（`list` 列出已保存的热点、`del <序号|名称>` 删除一个）、`ap`（热点状态、`ap on`、`ap off`）、
`time <Unix 秒>` 对时、`ble on` / `ble off`、`log`（看/改日志级别：`log warn` 设全局、
`log wifi info` 只打开某个模块、`log reset` 恢复默认；默认策略把 `wifi`/`wpa`
两个驱动 TAG 压到 warn，理由见 [wifi-provisioning 的选网小节](#记多个热点怎么选怎么退避)）；
另有**只能走 USB** 的 `shot`（见 [serial-screenshot.zh_CN.md](serial-screenshot.zh_CN.md)）、
`key`（注入一整套按键手势:短按 / 长按 / 连按两次）、`sleep`（触发浅/深睡，用于核对空闲行为）与 `debug on`。
手机连上并订阅通知后，设备会自动把 `help` 的输出推过去。

#### 从电脑上连

`tools/ble_console.py` 是同一套服务的终端，所以带蓝牙的电脑不用手机、不用热点就能调参：

```bash
pip install bleak
python3 tools/ble_console.py                 # 自动选第一个 LoveCount-*
python3 tools/ble_console.py LoveCount-8C5E  # 按广播名选
printf 'status\n' | python3 tools/ble_console.py   # 非交互，喂一批命令
```

前提是蓝牙已经打开（默认关闭，见下），扫描时会顺带打印 rssi，信号弱一眼能看出来。

蓝牙默认关闭，开关存在配置记录的 `ble_enabled` 字段里，可以从设备设置页、后台网页
或 `ble on` / `ble off` 命令三处切换。开启期间控制台任务还会盯空闲：**连续 5 分钟
无人连接**就把协议栈停掉并把 `ble_enabled` 写回 0，归还约 51 KB 堆。手机连着的时候
永不自动关闭。

关蓝牙必须在一个普通任务里做（`nimble_port_stop()` 是无超时等待 NimBLE host 任务
退出的），所以 `main/love_ble.c` 自带一个 4 KB 栈的控制台任务，它同时负责执行命令、
空闲判定和把输出按协商 MTU 分片通知。这个任务必须在 `nimble_port_init()` **之前**建
（NimBLE 一起来，堆就碎到拿不出一块连续 4 KB 了），而关蓝牙时它会**自己退出**，
把那 4 KB 还给系统堆。让它常驻的代价比看着大：本机实测"本次开机用过一次蓝牙"之后，
最大连续块从 69,632 掉到 34,816 字节，而 Wi-Fi 驱动发一帧正需要一大块连续内存。

### 这条控制台的安全边界

链路本身**没有认证**：NUS 的两个特征都没带 `_WRITE_ENC` / `_AUTHEN` 标志（`main/love_ble.c`），
也没有配置任何 NimBLE 的 Security Manager 字段，所以任何连上来的中央设备**不需要配对就能写命令**。
防线因此落在"命令侧"和"命令能碰到什么"上：

| 防护 | 为什么 |
| --- | --- |
| 蓝牙默认关闭；开着时无人连接满 5 分钟自动关栈 | 主人没打开它就根本没有链路可攻 |
| `key`、`sleep`、`shot`、`debug on` **仅 USB** | 否则近场陌生人就拿到"遥控器""强制它睡下去"或"让它整晚不睡"的能力 |
| `wifi …`、`ap off`、`time <秒>` 从蓝牙过来时**要求机身确认**：屏幕弹出要做什么，8 秒内短按确定才执行（长按确定或超时＝拒绝） | 这三类会改持久状态或把主人锁在外面。USB 侧不要求 —— 插着线本身就是物理接触 |
| 热点密码每台随机生成并存 NVS（显示在设备屏幕上） | 原先由 MAC 推导，任何看到 SSID 的人都能算出密码，再进没有任何鉴权的后台页 |

**明确保留的残余风险**：`status` 仍会打出已配置的 SSID、局域网地址以及主人自己的事件名与分类名；
攻击者仍可长期占住唯一连接，让射频与约 51KB 堆一直占着并挡住主人；链路层配对仍未启用，
因为它的兼容性代价很实在（iOS 只在特征声明加密时才配对，Android 的 BLE 串口 App 对 PIN 配对
支持不稳，而 `bleak` 在 macOS 上根本不支持配对）。
