#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() (
    local actionlint_bin
    local test_dir

    python3 tools/check_repo.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    "${actionlint_bin}" -color .github/workflows/*.yml

    test_dir="$(mktemp -d /tmp/ai-passport-host-tests.XXXXXX)"
    trap 'case "${test_dir}" in /tmp/ai-passport-host-tests.*) rm -rf -- "${test_dir}" ;; esac' EXIT

    # 编译并运行一个 host test。参数:<测试名> <头文件目录> <源文件...>
    # 测试名同时是 tests/<名>.c 与输出可执行文件名。
    run_host_test() {
        local name="$1"
        local include_dir="$2"
        shift 2
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"${include_dir}" \
            "tests/${name}.c" "$@" -o "${test_dir}/${name}"
        "${test_dir}/${name}"
    }

    run_host_test test_ui_pixel_math main main/ui_pixel_math.c
    # 控制台的行协议:BLE 串口一次写入可能只有半行,也可能一次带好几行,
    # 行尾在手机 App 上有三种写法,超长行必须整行丢弃而不是截断后执行。
    run_host_test test_love_console_line main main/love_console_line.c
    # 配置的版本迁移:全仓最容易静默清空用户数据的一段逻辑,必须有主机测试。
    run_host_test test_love_config main main/love_config.c main/love_date.c main/love_lunar.c
    # love_date.c 的农历事件会调 love_lunar,所以两个测试都要带上 love_lunar.c。
    run_host_test test_love_date main main/love_date.c main/love_lunar.c
    # 列表屏的显示序:分组、组序、组内排序与"农历超表排最后"都在这里钉住。
    run_host_test test_love_event_order main \
        main/love_event_order.c main/love_date.c main/love_lunar.c
    # 机身轮播:上/下键走到哪一页、一共几页、页码怎么编。纯逻辑,错一位就是页码乱跳。
    run_host_test test_love_view main main/love_view.c
    # 农历换算依赖 tools/gen_lunar_table.py 生成的表,同样按纯逻辑测。
    run_host_test test_love_lunar main main/love_lunar.c main/love_date.c
    run_host_test test_bsp_display_rounding components/bsp/src \
        components/bsp/src/bsp_display_rounding.c
    run_host_test test_bsp_es8311_sleep_check components/bsp/src \
        components/bsp/src/bsp_es8311_sleep_check.c

    # 下面四个要多个 -I 目录(或额外源码),run_host_test 只收一个,所以展开写。
    # 按键意图表:熄屏时按下(PRESS)不算动作、判定事件只负责亮屏 —— 真机上踩过的坑:
    # 一次物理按键会发 PRESS + CLICK 两个事件,PRESS 亮了屏,CLICK 就把页面翻了。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include -Imain \
        tests/test_love_key.c main/love_key.c -o "${test_dir}/test_love_key"
    "${test_dir}/test_love_key"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_button.c -o "${test_dir}/test_bsp_button"
    "${test_dir}/test_bsp_button"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_lvgl_init"
    "${test_dir}/test_bsp_lvgl_init"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_audio_recovery"
    "${test_dir}/test_bsp_audio_recovery"

    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    # 图标素材生成器：调色板 PNG 解码、降采样的取舍，以及"设备那张 16 色表一字不许改"
    # 这条不变量（新头像自带调色板，但老头像与内置图标仍然靠它，改了会让它们换色）。
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_love_pixel_art_gen.py
    # 头像像素化（取色 + 上色）的内核是纯 JS，用 node 的 vm 直接跑要发布的那份代码。
    # 没装 node 就明确跳过并写出来——不让"没跑"看起来像"跑过了"。
    if command -v node >/dev/null 2>&1; then
        node tests/test_avatar_pixel.mjs
    else
        echo "SKIP: 没有 node，未运行 tests/test_avatar_pixel.mjs"
    fi
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_archive_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_install_passport_skills.py
    echo "Host tests: PASS"
)

run_firmware_checks() (
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    validation_build_dir="$(mktemp -d /tmp/ai-passport-firmware.XXXXXX)"
    trap 'case "${validation_build_dir}" in /tmp/ai-passport-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac' EXIT

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${validation_build_dir}"
    PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
        "${validation_build_dir}" --archive-root "${repo_root}/build/firmware"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    echo "Firmware build: PASS"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
