// main/love_console_line.c —— 行协议实现,见 love_console_line.h 的说明。
#include "love_console_line.h"

void love_line_reset(love_line_t *line)
{
    if (!line) return;
    line->len = 0;
    line->overflow = false;
    line->skip_lf = false;
    line->pending = false;
    line->buf[0] = '\0';
}

bool love_line_feed(love_line_t *line, char c)
{
    if (!line) return false;

    // 上一个字节是 '\r',紧跟的 '\n' 是同一次按键在两个字节里的写法。
    // 这一句必须在自动复位之前:此时 pending 还置着,调用方读走的正是上一行。
    if (line->skip_lf) {
        line->skip_lf = false;
        if (c == '\n') return false;
    }

    // 上一行已经被取走了,这里开始新的一行。
    if (line->pending) love_line_reset(line);

    if (c == '\n' || c == '\r') {
        const bool crlf = c == '\r';
        if (line->len > 0 && !line->overflow) {
            // 保留 buf 供调用方读走,只清掉"正在收集"的状态。
            line->pending = true;
            line->skip_lf = crlf;
            return true;
        }
        // 空行,或刚被丢弃的超长行。
        love_line_reset(line);
        line->skip_lf = crlf;
        return false;
    }

    if (c == '\b' || c == 0x7F) {   // 退格 / DEL
        // 本行已经因超长被丢弃,退格不再把它救回来(长度已经不可信)。
        if (line->overflow) return false;
        if (line->len > 0) line->buf[--line->len] = '\0';
        return false;
    }

    if (c == '\t') c = ' ';

    // 其余控制字符(方向键转义序列、响铃等)一律忽略,不让它们混进参数里。
    if ((unsigned char)c < 0x20) return false;

    // 留一个字节给结尾 '\0'。
    if (line->len + 1 >= LOVE_LINE_MAX) {
        line->overflow = true;
        return false;
    }

    line->buf[line->len++] = c;
    line->buf[line->len] = '\0';
    return false;
}

love_cmd_status_t love_line_split(char *text, char *argv[], size_t argv_max, size_t *argc)
{
    if (argc) *argc = 0;
    if (!text || !argv || argv_max == 0) return LOVE_CMD_EMPTY;

    size_t count = 0;
    char *p = text;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        if (count == argv_max) return LOVE_CMD_TOO_MANY_ARGS;
        argv[count++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') p++;
        if (*p == '\0') break;
        *p++ = '\0';
    }

    if (count == 0) return LOVE_CMD_EMPTY;
    if (argc) *argc = count;
    return LOVE_CMD_OK;
}
