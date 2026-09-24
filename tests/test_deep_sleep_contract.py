#!/usr/bin/env python3
"""Static contracts for the terminal deep-sleep shutdown path."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def initializer(source: str, symbol: str) -> str:
    match = re.search(rf"\b{re.escape(symbol)}\s*\[\]\s*=\s*\{{", source)
    if not match:
        raise AssertionError(f"initializer not found: {symbol}")
    end = source.find("};", match.end())
    if end < 0:
        raise AssertionError(f"initializer is unterminated: {symbol}")
    return source[match.end():end]


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source)
    if not match:
        raise AssertionError(f"function not found: {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:index]
    raise AssertionError(f"function is unterminated: {name}")


def register_pairs(block: str) -> list[tuple[int, int]]:
    return [
        (int(reg, 16), int(value, 16))
        for reg, value in re.findall(r"\{\s*0x([0-9A-Fa-f]{2})\s*,\s*0x([0-9A-Fa-f]{2})\s*\}", block)
    ]


class DeepSleepContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.audio = read("components/bsp/src/bsp_audio.c")
        cls.battery = read("components/bsp/src/bsp_battery.c")
        cls.display = read("components/bsp/src/bsp_display.c")
        cls.i2c = read("components/bsp/src/bsp_i2c.c")
        cls.power_sleep = read("main/power_sleep.c")
        cls.love_app = read("main/love_app.c")
        cls.love_net = read("main/love_net.c")

    def test_es8311_force_sleep_sequence_is_complete_and_ordered(self) -> None:
        expected = [
            (0x32, 0x00), (0x17, 0x00), (0x0E, 0xFF), (0x12, 0x02),
            (0x14, 0x00), (0x0D, 0xFA), (0x15, 0x00), (0x02, 0x10),
            (0x00, 0x00), (0x00, 0x1F), (0x01, 0x30), (0x01, 0x00),
            (0x45, 0x01), (0x0D, 0xFC), (0x02, 0x00),
        ]
        actual = register_pairs(initializer(self.audio, "s_es8311_sleep_sequence"))
        self.assertEqual(actual, expected)

    def test_es8311_critical_registers_are_read_back(self) -> None:
        # The C host test exhausts all readback values against the actual policy.
        # This integration contract makes sure the driver uses it, and that an
        # I2C read failure cannot pass even when the output byte matches.
        body = function_body(self.audio, "es8311_force_sleep_once")
        self.assertIn("s_ctrl->write_reg", body)
        self.assertIn("s_ctrl->read_reg", body)
        self.assertIn("i < bsp_es8311_sleep_check_count", body)
        self.assertIn("&bsp_es8311_sleep_checks[i]", body)
        self.assertRegex(
            body,
            r"read_result == ESP_CODEC_DEV_OK\s*&&\s*"
            r"bsp_es8311_sleep_check_matches\(item, actual\)",
        )

    def test_es8311_force_sleep_retries_once_after_five_ms(self) -> None:
        self.assertRegex(self.audio, r"#define\s+ES8311_SLEEP_ATTEMPTS\s+2\b")
        self.assertRegex(self.audio, r"#define\s+ES8311_SLEEP_RETRY_MS\s+5\b")
        body = function_body(self.audio, "es8311_force_sleep")
        self.assertIn("attempt <= ES8311_SLEEP_ATTEMPTS", body)
        self.assertIn("pdMS_TO_TICKS(ES8311_SLEEP_RETRY_MS)", body)

    def test_audio_suspend_does_not_require_a_silent_open(self) -> None:
        body = function_body(self.audio, "bsp_audio_sleep")
        self.assertIn("es8311_force_sleep()", body)
        self.assertIn("audio_disable_i2s_channels()", body)
        self.assertNotIn("bsp_audio_set_format", body)

    def test_i2s_pins_are_released_only_by_deep_sleep_api(self) -> None:
        body = function_body(self.audio, "bsp_audio_prepare_deep_sleep")
        for pin in ("BSP_I2S_MCLK", "BSP_I2S_BCLK", "BSP_I2S_WS",
                    "BSP_I2S_DOUT", "BSP_I2S_DIN"):
            self.assertIn(pin, body)
        self.assertIn("GPIO_MODE_INPUT", body)
        self.assertIn("GPIO_PULLUP_DISABLE", body)
        self.assertIn("GPIO_PULLDOWN_DISABLE", body)

    def test_cw2017_sleep_is_verified_and_retried(self) -> None:
        body = function_body(self.battery, "bsp_battery_sleep")
        self.assertIn("attempt <= 2", body)
        self.assertIn("cw_write(CW_REG_CONFIG, CW_CONFIG_SLEEP)", body)
        self.assertIn("pdMS_TO_TICKS(5)", body)
        self.assertIn("cw_read(CW_REG_CONFIG, &actual, 1)", body)
        self.assertIn("actual == CW_CONFIG_SLEEP", body)

    def test_shared_i2c_is_released_after_device_transactions(self) -> None:
        body = function_body(self.i2c, "bsp_i2c_prepare_deep_sleep")
        self.assertIn("BSP_I2C_SDA", body)
        self.assertIn("BSP_I2C_SCL", body)
        self.assertIn("GPIO_MODE_INPUT", body)
        self.assertIn("GPIO_PULLUP_DISABLE", body)
        self.assertIn("GPIO_PULLDOWN_DISABLE", body)

    def test_lcd_safe_levels_and_holds_are_configured(self) -> None:
        pins = initializer(self.display, "s_deep_sleep_pins")
        levels = initializer(self.display, "s_deep_sleep_levels")
        self.assertRegex(pins, r"BSP_LCD_CS.*BSP_LCD_SCLK.*BSP_LCD_MOSI.*BSP_LCD_DC.*BSP_LCD_BL")
        self.assertRegex(levels, r"1\s*,\s*0\s*,\s*0\s*,\s*0\s*,\s*0")
        body = function_body(self.display, "bsp_display_prepare_deep_sleep")
        self.assertIn("esp_lcd_panel_disp_on_off(s_panel, false)", body)
        self.assertIn("esp_lcd_panel_disp_sleep(s_panel, true)", body)
        self.assertIn("ledc_stop", body)
        self.assertIn("gpio_hold_en", body)
        self.assertIn("gpio_deep_sleep_hold_en", body)

    def test_lcd_holds_are_released_before_spi_initialization(self) -> None:
        release = function_body(self.display, "display_release_deep_sleep_holds")
        self.assertIn("gpio_deep_sleep_hold_dis", release)
        self.assertIn("gpio_hold_dis", release)
        init = function_body(self.display, "bsp_display_init")
        self.assertLess(init.index("display_release_deep_sleep_holds()"),
                        init.index("spi_bus_initialize"))

    def test_terminal_shutdown_order_precedes_deep_sleep(self) -> None:
        body = function_body(self.power_sleep, "run_deep_sleep")
        calls = [
            "bsp_battery_sleep()",
            "bsp_audio_sleep()",
            "bsp_audio_prepare_deep_sleep()",
            "bsp_i2c_prepare_deep_sleep()",
            "bsp_display_prepare_deep_sleep()",
            "esp_deep_sleep_start()",
        ]
        positions = [body.index(call) for call in calls]
        self.assertEqual(positions, sorted(positions))
        self.assertLess(body.index("bsp_lvgl_lock(1000)"),
                        body.index("bsp_display_prepare_deep_sleep()"))

    def test_hotspot_intent_survives_deep_sleep_and_the_gate_still_wins(self) -> None:
        # 深睡唤醒＝重启:睡前开着热点的设备必须能自己把它开回来,否则配过网的用户醒来
        # 要等 60 秒兜底(见 love_net_poll),而"手动关过"那道闸一旦立着,兜底永远不开
        # ——那正是"睡一觉醒来三个入口全断"的用户现场。
        # 快照只能落在 RTC 保留内存里:普通静态变量过不了这一觉。写入还必须**先记再清**,
        # 顺序反了记下的就永远是 false。
        self.assertRegex(self.love_net, r"RTC_DATA_ATTR\s+\w+\s+s_ap_at_sleep_\w+")

        deinit = function_body(self.love_net, "love_net_deinit")
        self.assertLess(deinit.index("s_ap_at_sleep_on = s_ap_requested;"),
                        deinit.index("s_ap_requested = false;"))

        # 恢复那一条的判据:magic 对得上、睡前确实是开的、而且没有手动关闭的闸。
        # 三个条件少一个都会改变用户能看到的行为,所以逐个钉住(折叠换行后再比对)。
        init = re.sub(r"\s+", " ", function_body(self.love_net, "love_net_init"))
        self.assertIn(
            "else if (!s_ap_manual_off && s_ap_at_sleep_magic == AP_AT_SLEEP_MAGIC "
            "&& s_ap_at_sleep_on) {",
            init,
        )
        self.assertIn("s_ap_requested = true;", init)
        # 快照只认一次:留着它,之后每一次重启都会凭一份过期状态把热点开回来。
        self.assertIn("s_ap_at_sleep_magic = 0;", init)

    def test_wake_source_is_armed_and_checked_before_the_teardown(self) -> None:
        # 唤醒源要**先武装、再关外设**,而且返回值必须被检查:官方参考文档记过一个坑 ——
        # 忽略 esp_deep_sleep_enable_gpio_wakeup() 的返回值,芯片会在**没有任何唤醒源**的
        # 情况下睡下去,表现为"睡下去再也醒不来",只能断电救。所以武装失败的分支必须在
        # 任何一次性关外设之前就退出。
        body = function_body(self.power_sleep, "run_deep_sleep")
        # 按键采样必须停在武装之前:采样与 ADC 输入网络会给唤醒脚注入瞬变。
        self.assertLess(body.index("bsp_button_suspend()"), body.index("arm_button_wake()"))
        self.assertLess(body.index("arm_button_wake()"), body.index("bsp_battery_sleep()"))
        self.assertLess(body.index("arm_button_wake()"), body.index("esp_deep_sleep_start()"))
        # 失败路径(装回按键)排在所有关外设之前 —— 也就是"没配上唤醒源就不睡"。
        self.assertLess(body.index("bsp_button_resume()"), body.index("bsp_battery_sleep()"))

    def test_button_wake_uses_a_pin_bitmask_on_the_shared_adc_node(self) -> None:
        # 三个按键共用一个 ADC 节点,所以一条低电平唤醒源覆盖全部三个键。
        # 两个 GPIO API 的参数都是**位掩码**而不是脚号:传脚号等于空掩码,唤醒源装不上。
        body = function_body(self.power_sleep, "arm_button_wake")
        self.assertIn("1ULL << BSP_BTN_GPIO", body)
        self.assertIn("ESP_GPIO_WAKEUP_GPIO_LOW", body)
        self.assertIn("GPIO_MODE_INPUT", body)
        # 返回值必须往上传,由 run_deep_sleep 决定不睡。
        self.assertIn("return esp_deep_sleep_enable_gpio_wakeup(", body)

    def test_internal_sleep_resistors_are_disabled_for_the_external_pullup(self) -> None:
        # 板上给自己的 10k 上拉把唤醒脚抬到高电平。IDF 默认(CONFIG_...=y)会在入睡前按
        # 唤醒电平**再自动加一层内部上下拉**(esp_hw_support/sleep_modes.c 的
        # gpio_deep_sleep_wakeup_prepare),esp_sleep.h 对"外部上下拉 + 这个选项"有明文警告。
        # 这一行一旦被删掉,这个脚在睡眠期间的状态就有两个来源,而它没有别的测试能发现。
        defaults = read("sdkconfig.defaults")
        self.assertIn("CONFIG_ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS=n", defaults)

    def test_resolved_sdkconfig_agrees_when_it_exists(self) -> None:
        # 上面那条只钉住 defaults,而构建真正读的是被 gitignore 的本机 sdkconfig
        # (本仓库踩过"defaults 改了但 sdkconfig 陈旧、配置静默失效"的坑)。存在就一起核对。
        resolved = ROOT / "sdkconfig"
        if not resolved.is_file():
            return
        text = resolved.read_text(encoding="utf-8", errors="replace")
        self.assertIn("# CONFIG_ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS is not set", text)

    def test_idle_path_uses_the_official_five_minutes_and_no_timer(self) -> None:
        # 空闲路径的形态就是这次改动的目的:官方那 5 分钟,而且**不设定时器** ——
        # 睡到有人按键为止。定时唤醒会让设备每隔一段自己重启一次,那是重启循环不是休眠。
        self.assertRegex(self.love_app, r"#define\s+IDLE_DEEP_SLEEP_S\s+300\b")
        # 这里**不用** function_body():它靠正则配对花括号,而这个函数名在注释里也出现过
        # (`blank_off_poll` 的说明里就引了它),会取到隔壁函数的函数体(踩过)。用整句锚定。
        self.assertRegex(
            self.love_app,
            r"if \(love_app_sleep_deep\(0\) != ESP_OK\)",
        )
        # 三道闸门里的网页那一道:手机挂着后台页时即便屏幕已熄也不能睡。
        self.assertRegex(
            self.love_app,
            r"if \(love_httpd_client_idle_seconds\(\) < IDLE_DEEP_SLEEP_S\) return;",
        )

    def test_deep_sleep_arms_the_timer_only_when_a_duration_is_given(self) -> None:
        # wake_seconds = 0 必须真的**不**武装定时器:写反了 idle 路径就会变成每 5 秒自重启。
        body = function_body(self.power_sleep, "run_deep_sleep")
        before_start = body[:body.index("esp_deep_sleep_start()")]
        self.assertRegex(
            before_start,
            r"if \(s_deep_seconds != 0\)\s*\{[^}]*esp_sleep_enable_timer_wakeup",
        )

    def test_button_wake_leaves_the_shared_adc_node_without_internal_pulls(self) -> None:
        # bsp_pins.h 明文警告:这个分压节点不能改用内部上拉(约 45k、精度差,会把三档电平
        # 挤到一起并随温漂重叠)。所以武装唤醒时只能把脚配成输入,不能给它加内部上下拉。
        body = function_body(self.power_sleep, "arm_button_wake")
        self.assertIn("GPIO_PULLUP_DISABLE", body)
        self.assertIn("GPIO_PULLDOWN_DISABLE", body)
        self.assertNotIn("GPIO_PULLUP_ENABLE", body)


if __name__ == "__main__":
    unittest.main()
