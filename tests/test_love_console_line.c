#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "love_console_line.h"

// 逐字节喂一整段文本,返回攒出了几行;行内容按顺序抄进 lines。
static size_t feed_all(const char *text, char lines[][LOVE_LINE_MAX], size_t max_lines)
{
    love_line_t line;
    love_line_reset(&line);

    size_t count = 0;
    for (const char *p = text; *p != '\0'; p++) {
        if (love_line_feed(&line, *p)) {
            assert(count < max_lines);
            strcpy(lines[count++], line.buf);
        }
    }
    return count;
}

// 三种行尾写法必须都算一整行,而且 \r\n 只能算**一次**行尾 ——
// 手机 App 默认发 \r\n,当成两次的话每条命令后面都会多执行一个空行。
static void test_line_endings(void)
{
    char lines[4][LOVE_LINE_MAX];

    assert(feed_all("help\n", lines, 4) == 1);
    assert(strcmp(lines[0], "help") == 0);

    assert(feed_all("help\r\n", lines, 4) == 1);
    assert(strcmp(lines[0], "help") == 0);

    assert(feed_all("help\r", lines, 4) == 1);
    assert(strcmp(lines[0], "help") == 0);

    // 两行连在一起,中间是 \r\n。
    assert(feed_all("wifi a b\r\nhelp\n", lines, 4) == 2);
    assert(strcmp(lines[0], "wifi a b") == 0);
    assert(strcmp(lines[1], "help") == 0);
}

static void test_empty_lines_are_dropped(void)
{
    char lines[4][LOVE_LINE_MAX];

    // 空行、只有空白、连续换行都不产生可执行的行。
    assert(feed_all("\n", lines, 4) == 0);
    assert(feed_all("\r\n", lines, 4) == 0);
    assert(feed_all("\r\n\r\n\n", lines, 4) == 0);
    assert(feed_all("   \n", lines, 4) == 1);   // 空白行会吐出来,由 split 判成 EMPTY
}

// 超长行必须**整行丢弃**:如果只截断到 128 字节就当命令执行,
// "wifi mynet 密码后半截" 会被当成完整凭据写进 NVS,配网就再也连不上了。
static void test_overlong_line_is_dropped(void)
{
    char lines[4][LOVE_LINE_MAX];

    love_line_t line;
    love_line_reset(&line);

    // 喂进 3 倍上限的字符,期间不能吐出任何一行。
    for (int i = 0; i < LOVE_LINE_MAX * 3; i++) {
        assert(!love_line_feed(&line, 'x'));
    }
    // 行尾到达:这一行被丢弃,不算完整行。
    assert(!love_line_feed(&line, '\n'));

    // 紧接着的正常命令必须照常执行 —— 丢弃不能把后面的输入一起吃掉。
    char rest[] = "help\n";
    size_t count = 0;
    for (const char *p = rest; *p != '\0'; p++) {
        if (love_line_feed(&line, *p)) {
            strcpy(lines[count++], line.buf);
        }
    }
    assert(count == 1);
    assert(strcmp(lines[0], "help") == 0);
}

// 恰好放满的长度不能算超长。
static void test_max_length_boundary(void)
{
    love_line_t line;
    love_line_reset(&line);

    char expect[LOVE_LINE_MAX];
    memset(expect, 'a', sizeof(expect) - 1);
    expect[sizeof(expect) - 1] = '\0';

    for (int i = 0; i < LOVE_LINE_MAX - 1; i++) {
        assert(!love_line_feed(&line, 'a'));
    }
    assert(love_line_feed(&line, '\n'));
    assert(strcmp(line.buf, expect) == 0);

    // 再多一个字符就超长,整行作废。
    love_line_reset(&line);
    for (int i = 0; i < LOVE_LINE_MAX; i++) {
        assert(!love_line_feed(&line, 'a'));
    }
    assert(!love_line_feed(&line, '\n'));
}

static void test_backspace(void)
{
    char lines[4][LOVE_LINE_MAX];

    // 敲成 "helpX" 再退格删掉 X 得 "help"。
    assert(feed_all("helpX\b\n", lines, 4) == 1);
    assert(strcmp(lines[0], "help") == 0);

    // DEL(0x7F) 与 \b 等价。
    assert(feed_all("helpX\x7F\n", lines, 4) == 1);
    assert(strcmp(lines[0], "help") == 0);

    // 空行上退格不能把 len 减成负数(会读到 buf[-1])。这一步有 -Wall -Wextra
    // 也测不出越界,只能靠改坏实现时这里直接崩掉来发现。
    assert(feed_all("\b\b\b\n", lines, 4) == 0);

    // 退格删光了就回到空行,不会吐出可执行的行。
    assert(feed_all("ab\b\b\n", lines, 4) == 0);
}

static void test_control_chars_are_ignored(void)
{
    char lines[4][LOVE_LINE_MAX];

    // 手机 App 有可能会带上响铃、ESC 之类的控制字节,不能让它们混进参数。
    // (完整的方向键转义序列不用管:那条路只有 USB 终端,由 linenoise 处理。)
    assert(feed_all("he\x01lp\x1b\n", lines, 4) == 1);
    assert(strcmp(lines[0], "help") == 0);
}

static void test_split(void)
{
    char buf[LOVE_LINE_MAX];
    char *argv[LOVE_ARGV_MAX];
    size_t argc = 0;

    strcpy(buf, "wifi mynet secret");
    assert(love_line_split(buf, argv, LOVE_ARGV_MAX, &argc) == LOVE_CMD_OK);
    assert(argc == 3);
    assert(strcmp(argv[0], "wifi") == 0);
    assert(strcmp(argv[1], "mynet") == 0);
    assert(strcmp(argv[2], "secret") == 0);

    // 连续空格、前后空白、制表符都要吃掉。
    strcpy(buf, "  wifi   mynet\tsecret  ");
    assert(love_line_split(buf, argv, LOVE_ARGV_MAX, &argc) == LOVE_CMD_OK);
    assert(argc == 3);
    assert(strcmp(argv[2], "secret") == 0);

    strcpy(buf, "   ");
    assert(love_line_split(buf, argv, LOVE_ARGV_MAX, &argc) == LOVE_CMD_EMPTY);
    assert(argc == 0);

    strcpy(buf, "");
    assert(love_line_split(buf, argv, LOVE_ARGV_MAX, &argc) == LOVE_CMD_EMPTY);

    // 超参数上限必须报错,绝不能截断后执行(那会变成"少了密码的 wifi 命令")。
    strcpy(buf, "a b c d e f g h i");
    assert(love_line_split(buf, argv, LOVE_ARGV_MAX, &argc) == LOVE_CMD_TOO_MANY_ARGS);
    assert(argc == 0);

    // 恰好用满上限是合法的。
    strcpy(buf, "a b c d e f g h");
    assert(love_line_split(buf, argv, LOVE_ARGV_MAX, &argc) == LOVE_CMD_OK);
    assert(argc == LOVE_ARGV_MAX);
}

int main(void)
{
    test_line_endings();
    test_empty_lines_are_dropped();
    test_overlong_line_is_dropped();
    test_max_length_boundary();
    test_backspace();
    test_control_chars_are_ignored();
    test_split();

    printf("test_love_console_line: PASS\n");
    return 0;
}
