#!/usr/bin/env bash
#
# 适用平台：九联星闪开发板，海思 WS63E 芯片。
#
# 按同目录下《替换文件步骤.txt》将 HiHope 提供的 adc 相关文件覆盖到 OpenHarmony SDK 对应路径。
# 用法（在任意目录）:
#   bash apply_adc_driver_patch.sh              # 执行前需确认
#   bash apply_adc_driver_patch.sh --yes        # 跳过确认
#   bash apply_adc_driver_patch.sh --dry-run    # 仅打印将要执行的操作
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCH_DIR="$SCRIPT_DIR"

DRY_RUN=0
SKIP_CONFIRM=0
for a in "$@"; do
  case "$a" in
    --dry-run) DRY_RUN=1 ;;
    --yes|-y)  SKIP_CONFIRM=1 ;;
    -h|--help)
      echo "用法: bash $(basename "$0") [--yes] [--dry-run]"
      echo "  按《替换文件步骤.txt》覆盖 ws63 SDK 中 adc 相关目录与 adc.h。"
      exit 0
      ;;
  esac
done

find_oh_root() {
  local d="$1"
  while [[ "$d" != "/" ]]; do
    if [[ -d "$d/device/soc/hisilicon/ws63v100/sdk" ]]; then
      echo "$d"
      return 0
    fi
    d="$(dirname "$d")"
  done
  return 1
}

OH_ROOT="$(find_oh_root "$SCRIPT_DIR")" || {
  echo "[错误] 未找到 OpenHarmony 源码根（需包含 device/soc/hisilicon/ws63v100/sdk）。请将此脚本放在 ws63_ohos 工程树内使用。" >&2
  exit 1
}

SRC1="$PATCH_DIR/替换1/adc"
SRC2="$PATCH_DIR/替换2/adc"
SRC3="$PATCH_DIR/替换3/adc"
SRC4="$PATCH_DIR/替换4/adc.h"

DST1="$OH_ROOT/device/soc/hisilicon/ws63v100/sdk/drivers/chips/ws63/porting/adc"
DST2="$OH_ROOT/device/soc/hisilicon/ws63v100/sdk/drivers/drivers/driver/adc"
DST3="$OH_ROOT/device/soc/hisilicon/ws63v100/sdk/drivers/drivers/hal/adc"
DST4="$OH_ROOT/device/soc/hisilicon/ws63v100/sdk/include/driver/adc.h"

for s in "$SRC1" "$SRC2" "$SRC3" "$SRC4"; do
  if [[ ! -e "$s" ]]; then
    echo "[错误] 缺少补丁文件或目录: $s" >&2
    exit 1
  fi
done

echo "OpenHarmony 根目录: $OH_ROOT"
echo "补丁来源目录:       $PATCH_DIR"
echo
echo "将执行:"
echo "  1) rm -rf + cp -a  $SRC1  ->  $DST1"
echo "  2) rm -rf + cp -a  $SRC2  ->  $DST2"
echo "  3) rm -rf + cp -a  $SRC3  ->  $DST3"
echo "  4) cp -f           $SRC4  ->  $DST4"
echo

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[dry-run] 未修改任何文件。"
  exit 0
fi

if [[ "$SKIP_CONFIRM" -ne 1 ]]; then
  read -r -p "确认覆盖上述 SDK 文件？(y/N) " ans || true
  case "${ans:-}" in
    y|Y|yes|YES) ;;
    *) echo "已取消。"; exit 0 ;;
  esac
fi

do_replace_dir() {
  local src="$1" dst="$2"
  rm -rf "$dst"
  mkdir -p "$(dirname "$dst")"
  cp -a "$src" "$dst"
  echo "[完成] $dst"
}

do_replace_dir "$SRC1" "$DST1"
do_replace_dir "$SRC2" "$DST2"
do_replace_dir "$SRC3" "$DST3"
cp -f "$SRC4" "$DST4"
echo "[完成] $DST4"
echo
echo "全部替换完成。请重新编译固件验证 ADC 相关功能。"
