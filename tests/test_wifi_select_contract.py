#!/usr/bin/env python3
"""Static contracts for the Wi-Fi selection and fallback glue in `love_net.c`.

判据本身在 `love_net_pick.c`(由 `tests/test_love_net_pick.c` 覆盖)。这里只钉住那些
**只能留在胶水里**的东西 —— 它们碰不到主机测试,但每一条都对应过一次真机现场。
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


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


class WifiSelectContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.love_net = read("main/love_net.c")
        cls.pick_c = read("main/love_net_pick.c")

    def test_candidate_timeout_uses_the_signed_elapsed_helper(self) -> None:
        # 心跳在每次迭代**开头**取 now,而扫描结果回来时会在同一次迭代里启动新候选:
        # 那一刻 `since` 比 `now` 新,无符号相减下溢成一个巨大的正数,刚选出来的候选
        # 会在几毫秒内被当成"15 秒一个事件都没来"丢掉(真机日志里成对出现
        # "选网:连接信号最强的 X" 与 "候选 X:15 秒内没收到任何连接事件"),而且它的
        # "已试"位已经置上 —— 扫到的那张网一整轮都不会被真正试过。
        body = function_body(self.love_net, "love_net_poll")
        self.assertIn("love_net_pick_elapsed_ms(now, s_candidate_since)", body)
        self.assertNotRegex(body, r"\(\s*now\s*-\s*s_candidate_since\s*\)")
        # 差值要能表达"还没开始",再和超时比。
        self.assertIn("(int32_t)CONNECT_TIMEOUT_MS", body)

    def test_station_down_fallback_waits_for_a_pending_scan(self) -> None:
        # 兜底会把模式切到 APSTA,而 esp_wifi_set_mode() 会重启射频:撞上扫描,那 1~3 秒
        # 白扫,结果还可能整份作废。现场见过"扫描刚起步 → 兜底开热点 → 扫描回来一张都
        # 没有 → 退化成按保存顺序盲试"这条链。扫描只有 1~3 秒,等它落地再兜底。
        poll = function_body(self.love_net, "love_net_poll")
        self.assertRegex(
            poll,
            r"if \(s_saved_count > 0 && s_state != LOVE_NET_CONNECTED && !mid_round && !s_scan_pending\)",
        )

    def test_each_candidate_failure_stays_diagnosable(self) -> None:
        # "事件报了失败"与"15 秒里一个事件都没来"必须打得出来 —— 2026-09-24 的现场
        # 就是靠这两行分开 201(NO_AP_FOUND)与"驱动根本没回事件"的。
        self.assertIn("连不上(原因 %d)", self.love_net)
        self.assertIn("秒内没收到任何连接事件", self.love_net)

    def test_next_untried_is_scan_aware(self) -> None:
        # 换候选时要带上这一轮的扫描结果(看得见的优先),不能退回"只按保存顺序"。
        body = function_body(self.love_net, "advance_candidate")
        self.assertIn("love_net_pick_next_untried(ssids, s_saved_count, s_tried,", body)
        self.assertIn("s_scan, s_scan_count", body)


if __name__ == "__main__":
    unittest.main(verbosity=2)
