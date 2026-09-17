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
    # 农历换算依赖 tools/gen_lunar_table.py 生成的表,同样按纯逻辑测。
    run_host_test test_love_lunar main main/love_lunar.c main/love_date.c
    run_host_test test_bsp_display_rounding components/bsp/src \
        components/bsp/src/bsp_display_rounding.c
    run_host_test test_bsp_es8311_sleep_check components/bsp/src \
        components/bsp/src/bsp_es8311_sleep_check.c

    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
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
