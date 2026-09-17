// main/love_console.c —— USB 串口配网命令行。
//
// 存在的理由:配网本来只有两条路——连设备热点进后台网页、或走 BLE 对时那套。
// 前者每次都要先开热点再找地址,很麻烦;插着 USB 时直接敲一行就完事。
// 三条链路并存,这里只是多一条入口,不替代任何一条。
//
// 密码处理:命令只在终端里回显一次(本地配置不可避免),但**绝不写进日志、也不回读**。
// love_net_status_t 里唯一带密码的是 ap_pass,那是热点密码(由 MAC 派生、本来就印在
// 设备屏幕上),用户配置的那个密码只有写入路径、没有读出接口。
#include "love_console.h"

#include "love_net.h"

#include "esp_console.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "love_console";

static void print_status(void)
{
    love_net_status_t net;
    love_net_get_status(&net);

    printf("网络: %s", love_net_state_text(net.state));
    if (net.ip[0] != '\0') printf(", IP %s", net.ip);
    printf("\n");

    if (net.has_credentials) {
        printf("已配置 Wi-Fi: %s\n", net.sta_ssid);
    } else {
        printf("尚未配置 Wi-Fi。热点 %s 已开启,密码见设备屏幕。\n", net.ap_ssid);
    }

    const char *site = net.site_url[0] ? net.site_url
                     : (net.lan_url[0] ? net.lan_url : NULL);
    printf("后台网页: %s\n", site ? site : "暂不可达");
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc == 1) {
        print_status();
        return 0;
    }

    if (strcmp(argv[1], "clear") == 0) {
        if (love_net_forget() != ESP_OK) {
            printf("清除凭据失败。\n");
            return 1;
        }
        printf("凭据已清除,热点已打开,可以重新配网。\n");
        return 0;
    }

    // wifi open <名称> 连接开放网络;wifi <名称> <密码> 连接加密网络。
    const bool open = (strcmp(argv[1], "open") == 0);
    const int ssid_index = open ? 2 : 1;
    if (argc <= ssid_index || (!open && argc <= ssid_index + 1)) {
        printf("用法:\n"
               "  wifi                 查看当前状态\n"
               "  wifi <名称> <密码>   保存并连接\n"
               "  wifi open <名称>     连接开放网络\n"
               "  wifi clear           清除已保存的凭据\n"
               "名称与密码不能含空格。\n");
        return 1;
    }

    const char *ssid = argv[ssid_index];
    const char *pass = open ? "" : argv[ssid_index + 1];

    esp_err_t err = love_net_set_credentials(ssid, pass);
    if (err != ESP_OK) {
        printf("保存 \"%s\" 失败: %s。\n", ssid, esp_err_to_name(err));
        return 1;
    }
    printf("已保存 \"%s\"(%s),正在连接;稍后用 wifi 查看结果。\n",
           ssid, open ? "开放网络" : "已加密");
    return 0;
}

esp_err_t love_console_start(void)
{
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    // 不存历史:历史要挂文件系统(这里没有),也避免命令行(含密码)落到磁盘上。
    repl_config.max_history_len = 0;
    repl_config.history_save_path = NULL;
    repl_config.prompt = "> ";
    // 最长的合法命令行是「wifi <32 字节 SSID> <64 字节密码>」,128 足够。
    repl_config.max_cmdline_length = 128;
    repl_config.task_stack_size = 4096;
    repl_config.task_priority = 3;

    esp_console_dev_usb_serial_jtag_config_t dev_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_console_repl_t *repl = NULL;
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_config, &repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建串口控制台失败: %s", esp_err_to_name(err));
        return err;
    }

    const esp_console_cmd_t cmd = {
        .command = "wifi",
        .help = "配置 Wi-Fi:wifi <名称> <密码> / wifi open <名称> / wifi clear",
        .func = cmd_wifi,
    };
    err = esp_console_cmd_register(&cmd);
    if (err == ESP_OK) err = esp_console_register_help_command();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册控制台命令失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动串口控制台失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "USB 串口控制台已就绪:敲 wifi 查看用法");
    return ESP_OK;
}
