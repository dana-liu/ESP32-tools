#!/bin/bash
set -euo pipefail

if [[ $# -eq 0 || "$1" == "--help" ]]; then
  echo "用法：bash flash.sh <串口路径> 或 bash flash.sh --build-only <输出目录>"
  exit 0
fi

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sketch="$project_dir/firmware/esp32-terrarium"
fqbn='esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc'

if [[ "$1" == "--build-only" && $# -eq 2 ]]; then
  mkdir -p "$2"
  arduino-cli compile --fqbn "$fqbn" --output-dir "$2" "$sketch"
  exit
fi

if [[ $# -ne 1 || ! -c "$1" ]]; then
  echo "请指定有效串口，或使用 --build-only <输出目录>。" >&2
  exit 2
fi

port="$1"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/esp32-terrarium.XXXXXX")"
trap 'rm -r -- "$build_dir"' EXIT

if lsof "$port"; then
  echo "串口正在使用，请先退出串口推送器或监视器。" >&2
  exit 1
fi

arduino-cli compile --fqbn "$fqbn" --build-path "$build_dir" "$sketch"
arduino-cli upload --port "$port" --fqbn "$fqbn" --input-dir "$build_dir" "$sketch"
