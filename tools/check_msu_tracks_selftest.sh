#!/usr/bin/env bash
# MSU 音轨校验器的自测脚本。
#
# 在临时目录里伪造各种 .pcm / .opuz 文件，逐项验证 tools/check_msu_tracks.py
# 的退出码、报告内容和 JSON 结构：
#   正常包 / 坏 tag / 头部不足 8 字节 / 缺编号 / 可选 0 号 / 重复编号 /
#   非数字文件名 / 时长过短与过长 / repeat_position 越界 / 数据不对齐 /
#   空目录 / 目录不存在 / --quiet / --json / --ext opuz
#
# 用法: bash tools/check_msu_tracks_selftest.sh
# 退出码: 0 全部通过，1 有失败项。
set -u

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CHECKER="$SCRIPT_DIR/check_msu_tracks.py"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/msu-selftest.XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT

OUT="$WORK/stdout.txt"
ERR="$WORK/stderr.txt"
TOTAL=0
FAILED=0

ok() {
  TOTAL=$((TOTAL + 1))
  printf '  \033[32mPASS\033[0m %s\n' "$1"
}

ng() {
  TOTAL=$((TOTAL + 1))
  FAILED=$((FAILED + 1))
  printf '  \033[31mFAIL\033[0m %s\n' "$1"
  [ -s "$OUT" ] && sed 's/^/       | /' "$OUT"
  [ -s "$ERR" ] && sed 's/^/       ! /' "$ERR"
}

# expect <期望退出码> <说明> <命令...>
# 输出保存到 $OUT / $ERR，供后面的 contains/not_contains 使用。
expect() {
  local want="$1" label="$2"
  shift 2
  "$@" >"$OUT" 2>"$ERR"
  local rc=$?
  if [ "$rc" -eq "$want" ]; then
    ok "$label（退出码 $rc）"
  else
    ng "$label：期望退出码 $want，实际 $rc"
  fi
}

# 同时在 stdout 与 stderr 里找：目录级错误走 stderr，逐文件结果走 stdout。
contains() {
  if grep -qF -- "$2" "$OUT" || grep -qF -- "$2" "$ERR"; then
    ok "$1"
  else
    ng "$1：输出中找不到“$2”"
  fi
}

not_contains() {
  if grep -qF -- "$2" "$OUT" || grep -qF -- "$2" "$ERR"; then
    ng "$1：输出中不应出现“$2”"
  else
    ok "$1"
  fi
}

# make_pcm <路径> <秒数> [tag] [repeat_position]
# 头部 8 字节 + 静音 PCM（稀疏文件，几乎不占磁盘）。
make_pcm() {
  python3 - "$@" <<'PY'
import struct
import sys

path, seconds = sys.argv[1], float(sys.argv[2])
tag = sys.argv[3].encode("ascii") if len(sys.argv) > 3 else b"MSU1"
repeat = int(sys.argv[4]) if len(sys.argv) > 4 else 0
assert len(tag) == 4, "tag 必须 4 字节"
size = 8 + int(seconds * 44100) * 4
with open(path, "wb") as f:
    f.write(tag + struct.pack("<I", repeat))
    f.truncate(size)          # 稀疏扩展，内容为静音 0
PY
}

# make_opuz <路径> <包数量>
make_opuz() {
  python3 - "$@" <<'PY'
import struct
import sys

path, count = sys.argv[1], int(sys.argv[2])
with open(path, "wb") as f:
    f.write(b"OPUZ" + struct.pack("<I", 0))
    for i in range(count):
        payload = bytes([i & 0xFF]) * 10
        f.write(struct.pack("<H", len(payload)) + payload)
PY
}

# ---------------------------------------------------------------- 场景 1: 正常包
GOOD="$WORK/good"
mkdir -p "$GOOD"
make_pcm "$GOOD/1.pcm" 6
make_pcm "$GOOD/2.pcm" 5.5
make_pcm "$GOOD/3.pcm" 10
expect 0 "正常音轨包" python3 "$CHECKER" "$GOOD"
contains "正常包汇总为 3/3" "合格 3/3"
contains "正常包无缺失" "无缺失"
contains "表格含编号" "编号"
contains "表格时长格式正确" "0:10.0"
if ! grep -q "⚠" "$OUT"; then ok "正常包没有警告"; else ng "正常包不应有警告"; fi

# ---------------------------------------------------------------- 场景 2: 坏 tag
BADTAG="$WORK/badtag"
mkdir -p "$BADTAG"
make_pcm "$BADTAG/1.pcm" 6
make_pcm "$BADTAG/2.pcm" 6 RIFF
expect 1 "坏 tag 包失败" python3 "$CHECKER" "$BADTAG"
contains "坏 tag 报出实际值" "实际 b'RIFF'"
contains "坏 tag 说明不是 MSU-1 PCM" "这不是 MSU-1 PCM 文件"
contains "坏 tag 汇总含失败数" "失败 1"

# ---------------------------------------------------------------- 场景 3: 太短
SHORT="$WORK/short"
mkdir -p "$SHORT"
make_pcm "$SHORT/1.pcm" 6
printf 'MSU1' >"$SHORT/2.pcm"      # 只有 4 字节
: >"$SHORT/3.pcm"                  # 空文件也属于“不足 8 字节”
expect 1 "头部不足 8 字节失败" python3 "$CHECKER" "$SHORT"
contains "4 字节文件报出实际大小" "文件只有 4 字节，不足 8 字节头部"
contains "0 字节文件报出实际大小" "文件只有 0 字节，不足 8 字节头部"
contains "短文件汇总含失败数" "失败 2"

# ---------------------------------------------------------------- 场景 4: 缺编号
GAP="$WORK/gap"
mkdir -p "$GAP"
make_pcm "$GAP/1.pcm" 6
make_pcm "$GAP/2.pcm" 6
make_pcm "$GAP/4.pcm" 6
expect 0 "缺编号只是警告（退出码 0）" python3 "$CHECKER" "$GAP"
contains "列出缺失编号 3" "缺 1 个编号: 3"
contains "缺失时仍统计合格数" "合格 3/3"

# ---------------------------------------------------------------- 场景 5: 0 号可选
ZERO="$WORK/zero"
mkdir -p "$ZERO"
make_pcm "$ZERO/0.pcm" 4        # 4 秒菜单音轨：短且不报警
make_pcm "$ZERO/1.pcm" 6
make_pcm "$ZERO/2.pcm" 6
expect 0 "0 号可选且不参与缺失判断" python3 "$CHECKER" "$ZERO"
contains "有 0 号时无缺失" "无缺失"
contains "0 号被列出" "0.pcm"
not_contains "0 号短音轨不报 5 秒警告" "短于 5 秒"

# ---------------------------------------------------------------- 场景 6: 命名与重复
NAMES="$WORK/names"
mkdir -p "$NAMES"
make_pcm "$NAMES/1.pcm" 6
make_pcm "$NAMES/02.pcm" 6
make_pcm "$NAMES/2.pcm" 6       # 与 02 重复
make_pcm "$NAMES/intro.pcm" 6   # 非数字文件名
make_pcm "$NAMES/7.PCM" 6      # 大写扩展名也能识别
expect 0 "命名问题只是警告（退出码 0）" python3 "$CHECKER" "$NAMES"
contains "重复编号被指出" "编号 2 重复"
contains "非规范文件名被指出" "不是规范编号"
contains "非数字文件名被指出" "不是数字编号"
contains "大写扩展名被识别" "7.PCM"

# ---------------------------------------------------------------- 场景 7: 时长/循环点
EDGE="$WORK/edge"
mkdir -p "$EDGE"
make_pcm "$EDGE/1.pcm" 6
make_pcm "$EDGE/5.pcm" 2          # 非 0 号的过短音轨
make_pcm "$EDGE/2.pcm" 1801 MSU1  # 30 分 1 秒，稀疏文件
make_pcm "$EDGE/3.pcm" 6 MSU1 999999999   # repeat_position 越界
python3 - "$EDGE/4.pcm" <<'PY'
import struct
import sys
with open(sys.argv[1], "wb") as f:
    f.write(b"MSU1" + struct.pack("<I", 0) + b"\x00" * 101)
PY
expect 0 "超长/循环点/不对齐均为警告" python3 "$CHECKER" "$EDGE"
contains "超过 30 分钟给出警告" "超过 30 分钟"
contains "非 0 号过短音轨给出警告" "短于 5 秒"
contains "repeat_position 越界给出警告" "repeat_position=999999999"
contains "PCM 数据不对齐给出警告" "不是 4 的倍数"

# ---------------------------------------------------------------- 场景 8: 错误目录
EMPTY="$WORK/empty"
mkdir -p "$EMPTY"
expect 1 "空目录失败" python3 "$CHECKER" "$EMPTY"
contains "空目录说明原因" "没有找到 *.pcm 文件"
expect 1 "目录不存在失败" python3 "$CHECKER" "$WORK/no-such-dir"
contains "不存在的目录说明原因" "目录不存在"
expect 1 "路径是文件而非目录时失败" python3 "$CHECKER" "$GOOD/1.pcm"

# ---------------------------------------------------------------- 场景 9: --quiet
expect 1 "quiet 模式仍以退出码报告失败" python3 "$CHECKER" "$BADTAG" --quiet
contains "quiet 模式保留错误" "头部 tag 不是 MSU1"
contains "quiet 模式保留汇总" "失败 1"
not_contains "quiet 模式隐藏逐文件警告" "⚠"
if ! grep -q "1.pcm" "$OUT"; then ok "quiet 模式不打印正常文件行"; else ng "quiet 模式不应打印表格"; fi

# ---------------------------------------------------------------- 场景 10: --json
expect 1 "json 模式坏包退出码为 1" python3 "$CHECKER" "$BADTAG" --json
if python3 - "$OUT" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    data = json.load(f)
assert data["ext"] == "pcm"
assert data["expected_tag"] == "MSU1"
assert data["summary"]["total"] == 2, data["summary"]
assert data["summary"]["failed"] == 1
assert data["summary"]["ok"] is False
files = {item["name"]: item for item in data["files"]}
assert files["2.pcm"]["ok"] is False
assert files["2.pcm"]["errors"], files["2.pcm"]
assert files["1.pcm"]["repeat_position"] == 0
assert files["1.pcm"]["duration_seconds"] > 5
PY
then ok "json 输出结构正确"; else ng "json 输出结构不正确"; fi

expect 0 "json 模式正常包退出码 0" python3 "$CHECKER" "$GAP" --json
if python3 - "$OUT" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    data = json.load(f)
assert data["summary"]["ok"] is True
assert data["summary"]["missing_numbers"] == [3]
assert data["summary"]["total_duration_seconds"] >= 18
assert [item["number"] for item in data["files"]] == [1, 2, 4]
PY
then ok "json 汇总与缺失编号正确"; else ng "json 汇总与缺失编号不正确"; fi

# ---------------------------------------------------------------- 场景 11: --ext opuz
OPUS="$WORK/opus"
mkdir -p "$OPUS"
make_opuz "$OPUS/1.opuz" 3
make_opuz "$OPUS/2.opuz" 5
make_pcm "$OPUS/3.pcm" 6          # 与 --ext opuz 无关，应被忽略
expect 0 "opuz 音轨包合格" python3 "$CHECKER" "$OPUS" --ext opuz
contains "opuz 说明容器校验" "Opus"
contains "opuz 汇总 2/2" "合格 2/2"
not_contains "opuz 模式忽略 .pcm" "3.pcm"

BADOPUS="$WORK/badopus"
mkdir -p "$BADOPUS"
make_opuz "$BADOPUS/1.opuz" 2
make_pcm "$BADOPUS/2.opuz" 6      # MSU1 头，不是 OPUZ
expect 1 "opuz 模式拒绝 PCM 头" python3 "$CHECKER" "$BADOPUS" --ext opuz
contains "opuz 报出实际 tag" "实际 b'MSU1'"

# ---------------------------------------------------------------- 场景 12: 软链接
LINK="$WORK/link"
mkdir -p "$LINK"
make_pcm "$LINK/1.pcm" 6
make_pcm "$LINK/target.pcm" 6
ln -s target.pcm "$LINK/2.pcm"      # 指向包内文件的软链，正常
ln -s nowhere.pcm "$LINK/3.pcm"     # 断链：必须报错，不能只表现为“缺编号”
expect 1 "悬空软链接报错" python3 "$CHECKER" "$LINK"
contains "软链接音轨可正常校验" "2.pcm"
contains "断链指向的目标不存在" "符号链接指向的目标不存在"
contains "断链计入失败" "失败 1"

# ---------------------------------------------------------------- 场景 13: opuz 损坏
BROKEN="$WORK/brokenopus"
mkdir -p "$BROKEN"
make_opuz "$BROKEN/1.opuz" 2
python3 - "$BROKEN" <<'PY'
import os
import struct
import sys

directory = sys.argv[1]


def write(name, *chunks):
    with open(os.path.join(directory, name), "wb") as f:
        f.write(b"OPUZ" + struct.pack("<I", 0))
        for chunk in chunks:
            f.write(chunk)


write("2.opuz", struct.pack("<H", 5000), b"\x00" * 5000)   # 超过 1275 上限
write("3.opuz")                                            # 只有头部
write("4.opuz", struct.pack("<H", 100), b"\x00" * 10)      # 声明长度越界
PY
expect 1 "opuz 超限/无包/越界均失败" python3 "$CHECKER" "$BROKEN" --ext opuz
contains "opuz 包长度超上限被指出" "超过 1275 字节上限"
contains "opuz 只有头部被指出" "没有任何 Opus 数据包"
contains "opuz 数据包越界被指出" "数据包越界"

# ---------------------------------------------------------------- 场景 14: 目录错误
expect 1 "json 模式报告目录错误" python3 "$CHECKER" "$WORK/no-such-dir" --json
if python3 - "$OUT" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    data = json.load(f)
assert data["ok"] is False
assert "目录不存在" in data["error"], data
PY
then ok "json 目录错误结构正确"; else ng "json 目录错误结构不正确"; fi

# ---------------------------------------------------------------- 场景 15: 帮助
expect 0 "帮助可正常输出" python3 "$CHECKER" --help
contains "帮助含用法" "usage:"

echo
if [ "$FAILED" -eq 0 ]; then
  printf '\033[32m自测通过\033[0m：%d/%d 项通过\n' "$TOTAL" "$TOTAL"
  exit 0
else
  printf '\033[31m自测失败\033[0m：%d/%d 项失败\n' "$FAILED" "$TOTAL"
  exit 1
fi
