#!/usr/bin/env python3
"""MSU 音轨校验工具（零依赖，仅用 Python 标准库）。

扫描 MSU-1 音轨目录，逐文件检查本项目（app/jni/src/src/audio.c）的加载要求：

  * 文件头 8 字节：4 字节 tag + repeat_position（uint32 小端，单位是样本）
  * PCM 音轨：tag 必须是 b"MSU1"，数据为 44100 Hz / 16-bit signed /
    立体声交错（每采样 4 字节）
  * Opus 音轨（--ext opuz）：本项目加载器要求 tag 为 b"OPUZ"，
    数据包为 2 字节长度（低 15 位，上限 1275 字节）+ 载荷

检查项：
  1. 扩展名、文件大小（至少 8 字节）、头部 tag
  2. PCM 时长 (size - 8) / 4 / 44100；短于 5 秒或长于 30 分钟给出警告
     （0 号菜单/标题音轨较短属正常，不做下限检查）
  3. PCM 数据长度是否为 4 的倍数、repeat_position 是否落在音轨范围内
  4. 编号连续性（0 号是可选菜单/标题音轨；缺失只警告，不影响退出码）
  5. 汇总：总数、合格数、失败数、总时长、缺失编号

用法:
    python3 tools/check_msu_tracks.py
    python3 tools/check_msu_tracks.py /path/to/msu
    python3 tools/check_msu_tracks.py msu --quiet
    python3 tools/check_msu_tracks.py msu --ext opuz --json

选项:
    --ext pcm|opuz  选择校验的扩展名（默认 pcm）
    --quiet         只输出错误和汇总
    --json          输出 JSON（便于 CI/脚本消费）

退出码:
    0  所有音轨均合格（warning 不影响退出码）
    1  至少一个 error，或目录不存在、目录中没有可校验的音轨

说明:
    * 游戏按 "<编号>.pcm"（或 .opuz）拼文件名，所以文件名必须是数字编号；
      "007.pcm" 这类非规范命名、非数字命名、重复编号都会给出警告。
    * PCM 的 repeat_position 是样本偏移而不是字节偏移；加载器在其超出
      总样本数时会回绕，故这里给出警告。
    * Opus 的时长取决于解码结果，无法在不引入解码库的前提下精确计算，
      报告里显示为“未知”，也不参与时长合理性检查。
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import unicodedata
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

SAMPLE_RATE = 44100
BYTES_PER_SAMPLE = 4          # 16-bit 立体声交错：L(2) + R(2)
HEADER_SIZE = 8
MIN_DURATION = 5.0            # 秒；短于该值视为可疑
MAX_DURATION = 30 * 60.0      # 秒；长于该值视为可疑
MAX_OPUS_PACKET = 1275        # Opus 单包上限，加载器同样检查

# 各扩展名对应的文件头 tag。PCM 是 MSU-1 标准；Opus 的 tag 由本项目
# audio.c 的 opuz 分支决定（小端读成 'ZUPO'，即字节序列 OPUZ）。
EXPECTED_TAGS: Dict[str, bytes] = {
    "pcm": b"MSU1",
    "opuz": b"OPUZ",
}


class ScanError(Exception):
    """目录级错误（不存在、不可读、没有可校验文件）。"""


@dataclass
class TrackResult:
    """单个音轨的校验结果。"""

    path: Path
    name: str
    number: Optional[int] = None
    size: int = 0
    duration: Optional[float] = None
    repeat_position: Optional[int] = None
    total_samples: Optional[int] = None
    packet_count: Optional[int] = None
    errors: List[str] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        """没有 error 即视为合格文件（warning 不影响）。"""
        return not self.errors


@dataclass
class ScanResult:
    """整个目录的扫描结果。"""

    directory: Path
    ext: str
    tracks: List[TrackResult]
    missing: List[int]
    duplicates: Dict[int, List[str]]

    @property
    def failed(self) -> int:
        return sum(1 for track in self.tracks if not track.ok)

    @property
    def passed(self) -> int:
        return len(self.tracks) - self.failed

    @property
    def warning_count(self) -> int:
        return sum(len(track.warnings) for track in self.tracks)


def display_width(text: str) -> int:
    """终端显示宽度：CJK 全角字符占 2 列，用于表格对齐。"""
    return sum(2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
               for ch in text)


def pad(text: str, width: int, align: str = "left") -> str:
    """按显示宽度补空格（而不是按字符数）。"""
    fill = " " * max(0, width - display_width(text))
    return fill + text if align == "right" else text + fill


def human_size(size: int) -> str:
    """把字节数格式化成人类可读的大小。"""
    if size >= 1024 * 1024:
        return f"{size / (1024 * 1024):.1f} MB"
    if size >= 1024:
        return f"{size / 1024:.1f} KB"
    return f"{size} B"


def format_duration(seconds: Optional[float], tenths: bool = True) -> str:
    """格式化为 M:SS.s（或 H:MM:SS.s）；None 显示为“未知”。"""
    if seconds is None:
        return "—"
    seconds = max(0.0, seconds)
    if tenths:
        total_tenths = int(round(seconds * 10))
        hours, rem = divmod(total_tenths, 36000)
        minutes, rem = divmod(rem, 600)
        secs, tenth = divmod(rem, 10)
        if hours:
            return f"{hours}:{minutes:02d}:{secs:02d}.{tenth}"
        return f"{minutes}:{secs:02d}.{tenth}"
    total = int(round(seconds))
    hours, rem = divmod(total, 3600)
    minutes, secs = divmod(rem, 60)
    if hours:
        return f"{hours}:{minutes:02d}:{secs:02d}"
    return f"{minutes}:{secs:02d}"


def format_number_list(numbers: Sequence[int], limit: int = 16) -> str:
    """用中文顿号列出编号；过长时截断，避免刷屏。"""
    if not numbers:
        return "无"
    shown = "、".join(str(n) for n in numbers[:limit])
    if len(numbers) > limit:
        shown += f" …（共 {len(numbers)} 个）"
    return shown


def parse_track_number(name: str) -> Optional[int]:
    """从文件名解析编号；游戏只认纯 ASCII 数字（"1.pcm"）。"""
    stem = Path(name).stem
    if stem.isascii() and stem.isdigit():
        return int(stem)
    return None


def check_file(path: Path, ext: str) -> TrackResult:
    """校验单个音轨文件的内容（不负责编号连续性）。"""
    result = TrackResult(path=path, name=path.name)
    result.number = parse_track_number(path.name)

    try:
        result.size = path.stat().st_size
    except OSError as exc:
        result.errors.append(f"无法读取文件信息: {exc.strerror or exc}")
        return result

    if result.size < HEADER_SIZE:
        result.errors.append(
            f"文件只有 {result.size} 字节，不足 8 字节头部"
        )
        return result

    try:
        with path.open("rb") as handle:
            header = handle.read(HEADER_SIZE)
    except OSError as exc:
        result.errors.append(f"无法打开文件: {exc.strerror or exc}")
        return result

    if len(header) < HEADER_SIZE:
        # 一般只有 stat 与 read 之间文件被截断才会发生
        result.errors.append(
            f"文件只有 {len(header)} 字节，不足 8 字节头部"
        )
        return result

    tag = header[:4]
    result.repeat_position = struct.unpack("<I", header[4:8])[0]

    expected = EXPECTED_TAGS[ext]
    if tag != expected:
        if ext == "pcm":
            result.errors.append(
                f"头部 tag 不是 MSU1（实际 {tag!r}），这不是 MSU-1 PCM 文件"
            )
        else:
            result.errors.append(
                f"头部 tag 不是 OPUZ（实际 {tag!r}），这不是本项目的 MSU-1 Opus 音轨"
            )
        return result

    if ext == "pcm":
        check_pcm(result)
    else:
        check_opuz(result)
    return result


def check_pcm(result: TrackResult) -> None:
    """PCM 专项检查：时长、数据对齐、循环点。"""
    data_size = result.size - HEADER_SIZE
    result.total_samples = data_size // BYTES_PER_SAMPLE
    result.duration = data_size / BYTES_PER_SAMPLE / SAMPLE_RATE

    if data_size % BYTES_PER_SAMPLE:
        result.warnings.append(
            f"PCM 数据长度 {data_size} 字节不是 4 的倍数，"
            f"末尾 {data_size % BYTES_PER_SAMPLE} 字节会被忽略"
        )

    # 0 号是菜单/标题音轨，本来就短，不套用 5 秒下限。
    if result.duration <= MIN_DURATION and result.number != 0:
        result.warnings.append(
            f"时长仅 {result.duration:.1f} 秒，短于 5 秒，"
            "可能是截断文件或音效"
        )
    elif result.duration >= MAX_DURATION:
        result.warnings.append(
            f"时长 {format_duration(result.duration)} 超过 30 分钟，"
            "确认是否为拼接音轨"
        )

    if result.repeat_position is not None and result.total_samples is not None:
        if result.repeat_position > result.total_samples:
            result.warnings.append(
                f"repeat_position={result.repeat_position} 超出总样本数 "
                f"{result.total_samples}，循环点无效（加载器会回绕）"
            )
        elif result.repeat_position == result.total_samples and result.total_samples > 0:
            result.warnings.append(
                f"repeat_position={result.repeat_position} 指向音轨末尾，"
                "循环时会立即结束"
            )


def check_opuz(result: TrackResult) -> None:
    """Opus 专项检查：只校验容器分包，时长无法在零依赖下精确计算。"""
    offset = HEADER_SIZE
    packets = 0
    try:
        with result.path.open("rb") as handle:
            handle.seek(HEADER_SIZE)
            while offset < result.size:
                raw = handle.read(2)
                if len(raw) < 2:
                    result.warnings.append(
                        f"文件末尾有 {len(raw)} 字节残缺的数据包长度字段"
                    )
                    break
                length = struct.unpack("<H", raw)[0] & 0x7FFF
                if length > MAX_OPUS_PACKET:
                    result.errors.append(
                        f"第 {packets + 1} 个数据包长度 {length} 超过 "
                        f"{MAX_OPUS_PACKET} 字节上限"
                    )
                    break
                if offset + 2 + length > result.size:
                    result.errors.append(
                        f"第 {packets + 1} 个数据包越界（偏移 {offset}，"
                        f"长度 {length}，文件共 {result.size} 字节）"
                    )
                    break
                handle.seek(length, os.SEEK_CUR)
                offset += 2 + length
                packets += 1
    except OSError as exc:
        result.errors.append(f"读取 Opus 数据包失败: {exc.strerror or exc}")
        return

    result.packet_count = packets
    if packets == 0:
        result.errors.append("文件只有头部，没有任何 Opus 数据包")
    # Opus 的时长取决于解码器输出，和 (size-8)/4/44100 无关；
    # 不引入 libopus 就不做无依据的估算，duration 保持 None。


def check_numbering(tracks: Sequence[TrackResult], ext: str) -> Dict[int, List[str]]:
    """回填编号，检查命名规范与重复编号，返回重复编号表。"""
    by_number: Dict[int, List[TrackResult]] = {}
    for track in tracks:
        number = parse_track_number(track.name)
        track.number = number
        if number is None:
            track.warnings.append(
                f"文件名“{track.name}”不是数字编号，游戏按 "
                f"<编号>.{ext} 查找，不会加载该文件"
            )
            continue
        by_number.setdefault(number, []).append(track)
        if track.path.stem != str(number):
            track.warnings.append(
                f"文件名“{track.name}”不是规范编号，游戏会查找 "
                f"{number}.{ext}"
            )

    duplicates: Dict[int, List[str]] = {
        number: [track.name for track in group]
        for number, group in by_number.items()
        if len(group) > 1
    }
    for number, names in duplicates.items():
        for track in by_number[number]:
            others = "、".join(name for name in names if name != track.name)
            track.warnings.append(
                f"编号 {number} 重复（还存在 {others}），游戏只会加载其中一个"
            )
    return duplicates


def scan_dir(directory: Path, ext: str) -> ScanResult:
    """扫描目录，返回逐文件结果、缺失编号与重复编号。"""
    if not directory.exists():
        raise ScanError(f"目录不存在: {directory}")
    if not directory.is_dir():
        raise ScanError(f"不是目录: {directory}")

    suffix = "." + ext.lower()
    broken_links: List[Path] = []
    try:
        entries: List[Tuple[Optional[int], Path]] = []
        for entry in os.scandir(directory):
            path = Path(entry.path)
            if path.suffix.lower() != suffix:
                continue
            # 悬空软链接会被 is_file() 静默跳过；音轨包常用软链接指向外置
            # 存储，链接断了必须报错，否则只会表现为“编号缺失”。
            if entry.is_symlink() and not path.exists():
                broken_links.append(path)
                continue
            if not entry.is_file():
                continue
            entries.append((parse_track_number(path.name), path))
    except OSError as exc:
        raise ScanError(f"无法读取目录 {directory}: {exc.strerror or exc}") from exc

    if not entries and not broken_links:
        raise ScanError(f"目录 {directory} 下没有找到 *{suffix} 文件")

    # 先按编号排序（0 号菜单音轨在最前），非数字名排在最后。
    entries.sort(key=lambda item: (
        item[0] is None,
        item[0] if item[0] is not None else 0,
        item[1].name.lower(),
    ))

    tracks = [check_file(path, ext) for _, path in entries]
    for path in broken_links:
        broken = TrackResult(path=path, name=path.name)
        broken.number = parse_track_number(path.name)
        broken.errors.append("符号链接指向的目标不存在（链接已断开）")
        tracks.append(broken)

    # 编号解析、规范文件名检查、重复编号检查。
    duplicates = check_numbering(tracks, ext)

    # 连续性：0 号是可选菜单/标题音轨，不计入缺失；从 1 检查到最大编号。
    missing: List[int] = []
    present = {track.number for track in tracks if track.number is not None}
    if present:
        missing = [n for n in range(1, max(present) + 1) if n not in present]

    return ScanResult(
        directory=directory,
        ext=ext,
        tracks=tracks,
        missing=missing,
        duplicates=duplicates,
    )


def total_duration(tracks: Sequence[TrackResult]) -> Tuple[float, int]:
    """返回（已知时长之和，未知时长的合格文件数）。"""
    known = 0.0
    unknown = 0
    for track in tracks:
        if track.duration is not None:
            known += track.duration
        elif track.ok:
            unknown += 1
    return known, unknown


def scan_to_json(scan: ScanResult) -> Dict[str, Any]:
    """构造稳定的 JSON 结构，便于 CI 断言。"""
    known, unknown = total_duration(scan.tracks)
    has_duration = any(track.duration is not None for track in scan.tracks)
    return {
        "directory": str(scan.directory),
        "ext": scan.ext,
        "expected_tag": EXPECTED_TAGS[scan.ext].decode("ascii"),
        "files": [
            {
                "number": track.number,
                "name": track.name,
                "path": str(track.path),
                "size": track.size,
                "duration_seconds": (
                    round(track.duration, 3)
                    if track.duration is not None else None
                ),
                "duration": (
                    format_duration(track.duration)
                    if track.duration is not None else None
                ),
                "repeat_position": track.repeat_position,
                "total_samples": track.total_samples,
                "packet_count": track.packet_count,
                "ok": track.ok,
                "errors": track.errors,
                "warnings": track.warnings,
            }
            for track in scan.tracks
        ],
        "summary": {
            "total": len(scan.tracks),
            "passed": scan.passed,
            "failed": scan.failed,
            "total_duration_seconds": round(known, 3) if has_duration else None,
            "total_duration": (
                format_duration(known, tenths=False) if has_duration else None
            ),
            "duration_unknown_files": unknown,
            "missing_numbers": scan.missing,
            "duplicate_numbers": sorted(scan.duplicates),
            "warning_count": scan.warning_count,
            "ok": scan.failed == 0,
        },
    }


def report(scan: ScanResult, quiet: bool = False, as_json: bool = False) -> int:
    """输出报告并返回退出码（0 全部合格，1 存在 error）。"""
    if as_json:
        print(json.dumps(scan_to_json(scan), ensure_ascii=False, indent=2))
        return 0 if scan.failed == 0 else 1

    directory = str(scan.directory).rstrip(os.sep) + os.sep
    if scan.ext == "pcm":
        detail = "PCM · 44100 Hz · 16-bit · 立体声"
    else:
        detail = "Opus · 容器分包校验（时长未知）"
    print(f"MSU 音轨校验: {directory}  （{detail}）")

    # 表格：编号、文件、大小、时长
    if not quiet:
        numbers = [len(str(t.number)) for t in scan.tracks
                   if t.number is not None]
        number_width = max([4] + numbers)
        name_width = max(4, max(display_width(t.name) for t in scan.tracks))
        size_width = max(6, max(display_width(human_size(t.size))
                                for t in scan.tracks))
        duration_width = max(6, max(display_width(format_duration(t.duration))
                                    for t in scan.tracks))
        print("  {}   {}   {}   {}".format(
            pad("编号", number_width, "right"),
            pad("文件", name_width),
            pad("大小", size_width, "right"),
            pad("时长", duration_width, "right"),
        ))
        for track in scan.tracks:
            number = str(track.number) if track.number is not None else "—"
            print("  {}   {}   {}   {}".format(
                pad(number, number_width, "right"),
                pad(track.name, name_width),
                pad(human_size(track.size), size_width, "right"),
                pad(format_duration(track.duration), duration_width, "right"),
            ))

    # 错误始终输出；--quiet 下省略逐文件警告（只看错误和汇总）。
    issue_lines: List[str] = []
    for track in scan.tracks:
        for message in track.errors:
            issue_lines.append(f"  ❌ {track.name}: {message}")
    if not quiet:
        for track in scan.tracks:
            for message in track.warnings:
                issue_lines.append(f"  ⚠ {track.name}: {message}")
    if issue_lines:
        print()
        for line in issue_lines:
            print(line)

    known, unknown = total_duration(scan.tracks)
    has_duration = any(track.duration is not None for track in scan.tracks)
    if has_duration:
        duration_text = format_duration(known, tenths=False)
        if unknown:
            duration_text += f"（另 {unknown} 个时长未知）"
    else:
        duration_text = "未知"

    parts = []
    if scan.failed:
        parts.append(f"合格 {scan.passed}/{len(scan.tracks)}（失败 {scan.failed}）")
    else:
        parts.append(f"合格 {scan.passed}/{len(scan.tracks)}")
    parts.append(f"总时长 {duration_text}")
    if scan.missing:
        parts.append(f"缺 {len(scan.missing)} 个编号: "
                     f"{format_number_list(scan.missing)}")
    else:
        parts.append("无缺失")
    if scan.warning_count and not quiet:
        parts.append(f"{scan.warning_count} 条警告")

    if issue_lines or not quiet:
        print()
    print("  " + "，".join(parts))
    if scan.missing:
        if any(track.number == 0 for track in scan.tracks):
            hint = "缺失编号在游戏中会回退到原版音乐。"
        else:
            hint = "0 号为可选的菜单/标题音轨；缺失编号在游戏中会回退到原版音乐。"
        print(f"  提示: {hint}")

    return 0 if scan.failed == 0 else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="check_msu_tracks.py",
        description="校验 MSU-1 音轨目录（PCM/Opus）是否符合本项目的加载要求。",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "示例:\n"
            "  python3 tools/check_msu_tracks.py\n"
            "  python3 tools/check_msu_tracks.py msu --quiet\n"
            "  python3 tools/check_msu_tracks.py /path/to/msu --ext opuz --json"
        ),
    )
    parser.add_argument(
        "directory", nargs="?", default="msu", metavar="目录",
        help="音轨目录（默认: msu）",
    )
    parser.add_argument(
        "--ext", choices=("pcm", "opuz"), default="pcm", metavar="pcm|opuz",
        help="校验的扩展名（默认: pcm）",
    )
    parser.add_argument(
        "--quiet", action="store_true",
        help="只输出错误和汇总",
    )
    parser.add_argument(
        "--json", action="store_true",
        help="输出 JSON 格式结果（便于 CI/脚本消费）",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    directory = Path(args.directory)
    try:
        scan = scan_dir(directory, args.ext)
    except ScanError as exc:
        if args.json:
            print(json.dumps({
                "directory": str(directory),
                "ext": args.ext,
                "error": str(exc),
                "ok": False,
            }, ensure_ascii=False, indent=2))
        else:
            print(f"❌ {exc}", file=sys.stderr)
        return 1
    return report(scan, quiet=args.quiet, as_json=args.json)


if __name__ == "__main__":
    sys.exit(main())
