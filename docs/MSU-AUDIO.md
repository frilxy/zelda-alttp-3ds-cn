# MSU 音频使用指南```
sdmc:/3ds/Zelda 3DS/
  ├── zelda3.ini
  ├── zelda3_assets.dat
  └── msu/
      ├── alttp_msu-0.pcm     (可选，菜单曲)
      ├── alttp_msu-1.pcm
      ├── alttp_msu-2.pcm
      └── ...
```
本指南面向最终用户，说明如何在本移植版中启用 MSU-1 音频：把音轨放好即可，
程序会在启动时自动检测并切换采样率。**MSU 音轨不随发布包提供**，需要你自行准备。

> 适用范围：`sdmc:/3ds/Zelda 3DS/` 下的 CIA / 3DSX 版本（Old 3DS 与 New 3DS）。
> 游戏内下屏设置里没有 MSU 开关——它由文件和 `zelda3.ini` 决定。

---

## 1. MSU 音频是什么

原版《众神的三角力量》的音乐由 SNES 的 SPC700 音源实时合成，受 64 KB
采样内存与那个年代的限制。MSU-1 音频把这个位置换成从 SD 卡流式读取的高音质
音轨，于是同一段旋律可以有接近 CD 的质感。

| | 原版 SPC 音乐 | MSU 音频 |
|---|---|---|
| 音源 | SPC700 实时合成 | 44100 Hz / 16-bit 立体声 PCM |
| 音质 | 受 SNES 采样内存限制 | 高音质，接近原始编曲 |
| 体积 | 极小（在 `zelda3_assets.dat` 内） | 每分钟约 10 MB |
| 音效 | 引擎合成 | 引擎合成，MSU 只提供音乐轨 |
| 曲目 | 内置 | `1.pcm` ~ `48.pcm`，按编号对应 |

两点常被误解：

- **MSU 只提供音乐。** 挥剑、开箱、受伤等音效仍由游戏引擎负责，不由音轨提供。
- **可以只放一部分。** 缺哪个编号，那一首就自动回退到原版音乐，不会静音、
  也不会卡住。

---

## 2. 前置条件

| 项目 | 要求 | 说明 |
|---|---|---|
| 主机 | Old 3DS / New 3DS 系列 | 两者都支持，采样率策略不同，见第 6 节 |
| 音频固件 | `sdmc:/3ds/dspfirm.cdc` | **不在** `Zelda 3DS` 目录里；缺失会启动失败 |
| ROM | 干净的美版 `.sfc` / `.smc` | 本移植版用它建立游戏 profile，并从内置资源取中文 |
| 音轨 | 自备 MSU-1 音轨包 | 发布包内不含，版权原因 |
| SD 卡 | 建议 Class 10 / A1 以上 | 音轨是流式读取的，卡慢会影响切曲 |

获取 DSP 固件：打开 Rosalina（`L + 下 + Select`）→ Miscellaneous options →
Dump DSP firmware，重启后生效。

> 已安装过本移植版并正常出声的机器，`dspfirm.cdc` 通常已经就位，
> 可以直接跳到第 3 节。

---

## 3. 启用步骤

### 3.1 准备音轨

从你合法获得的渠道取得 MSU-1 音轨包（常见的是 `1.pcm` ~ `48.pcm` 的整套，
个别包还带 `0.pcm`）。解压后先别急着拷进 SD 卡，先做一次校验。

### 3.2 校验格式

每首音轨必须满足：

| 项目 | 要求 |
|---|---|
| 文件头 | 8 字节：`MSU1` 四个字符 + `repeat_position`（uint32 小端，单位是采样点） |
| 采样率 | 44100 Hz |
| 位深 | 16-bit signed |
| 声道 | 立体声，左右交错（每个采样 4 字节） |

仓库自带零依赖校验脚本（在电脑上运行，只用 Python 标准库，不需要装任何东西）：

```sh
python3 tools/check_msu_tracks.py msu
```

输出示例（含一个坏文件）：

```text
MSU 音轨校验: msu/  （PCM · 44100 Hz · 16-bit · 立体声）
  编号   文件        大小     时长
     0   0.pcm   689.1 KB   0:04.0
     1   1.pcm     1.0 MB   0:06.0
     2   2.pcm   947.5 KB   0:05.5
     3   3.pcm     1.0 MB        —

  ❌ 3.pcm: 头部 tag 不是 MSU1（实际 b'RIFF'），这不是 MSU-1 PCM 文件

  合格 3/4（失败 1），总时长 0:16，无缺失
```

脚本会检查头部 tag、文件长度是否对齐、`repeat_position` 是否越界、
编号是否连续、命名是否规范（必须是纯数字，`01.pcm` 之类只给警告）。
退出码 `0` 表示全部合格，`1` 表示至少一个文件不合格。

> 它只能验证文件**结构**，无法判断真实采样率。如果你手上的音轨是从别的格式
> 转来的，请自行确认转换时用的是 44100 Hz，否则会出现第 7 节的变速问题。

### 3.3 放置文件

把音轨放进 `sdmc:/3ds/Zelda 3DS/msu/`：

```text
sdmc:/3ds/Zelda 3DS/
├── zelda3.ini
├── zelda3_assets.dat
├── runtime.log
├── dumps/
├── msu/
│   ├── 0.pcm        可选：菜单 / 标题音轨
│   ├── 1.pcm
│   ├── 2.pcm
│   └── ...
├── profiles/
│   └── zelda3-3a1b2c3d/     每份 ROM 一个 profile（名称含 ROM 指纹）
│       ├── zelda3.ini       这一份才是启动时生效的配置
│       └── zelda3_assets.dat
└── saves/
    └── zelda3-3a1b2c3d/
```

要点：

- **标准包直接用原命名**（`alttp_msu-1.pcm`），不需要改名。
- `MSUPath` 是**路径前缀**，不是目录：代码按 `<前缀><编号>.pcm` 拼接。
  默认 `msu/alttp_msu-` → `msu/alttp_msu-1.pcm`；纯数字命名用 `msu/`。
- 编号前不要补零（`alttp_msu-01.pcm` 会匹配不到）。
  （检测与加载都只认纯数字文件名，扩展名请用小写 `.pcm`。）
- `0.pcm` 可选的菜单曲；不放也没关系。
- 目录名按 `msu` 书写，与默认 `MSUPath = msu/alttp_msu-` 保持一致。

### 3.4 确认生效

启动游戏后，用文本编辑器打开 `sdmc:/3ds/Zelda 3DS/runtime.log`，搜索 `MSU`：

```text
Audio config: 44100 Hz stereo, MSU on (enable=0x01, tracks found)
Audio initialized: 44100 Hz, 2 channels, 1024 samples
MSU: loading track 1 from msu/1.pcm
```

- 第一行出现 `MSU on ... tracks found` → 自动检测成功，采样率已切到 44100。
- 出现 `MSU off ... tracks not found` → 四个探测位置都没找到音轨。
- 出现 `MSU: track N not found at ... (falling back to SPC)` → 第 N 首打不开，
  已回退原版音乐（可能是 `MSUPath` 指错，见 7.1）。
- 出现 `Audio: dropping Opuz flag` → ini 里的 `opuz` 已被清除，按 PCM 播放。

`runtime.log` **每次启动都会重建**，所以只看最近一次运行。

### 3.5 可选：手动配置

默认配置（打进 CIA 的 `romfs:/zelda3.ini`，新 profile 会继承）已经包含：

```ini
[Sound]
EnableMSU = false
MSUPath = msu/alttp_msu-
ResumeMSU = 0
MSUVolume = 100%
```

通常不需要改动。要调节音量或续播行为时，编辑
`profiles/<rom>-<hash>/zelda3.ini`（**不是**根目录的 `zelda3.ini`，见第 5 节）。

---

## 4. 目录结构图

上面 3.3 节的图就是完整布局。补充说明：

- 程序启动时 `chdir()` 到 `sdmc:/3ds/Zelda 3DS/`，所有相对路径都以此为基准。
- `MSUPath = msu/alttp_msu-` 是相对这个工作目录的前缀，因此实际读取
  `sdmc:/3ds/Zelda 3DS/msu/1.pcm`。
- `runtime.log` 位于工作目录根部，不在 `msu/` 内。

---

## 5. `zelda3.ini` 配置说明

配置写在 `[Sound]` 段。键名**大小写不敏感**，`#` 之后是注释。

| 键 | 取值 | 默认 | 作用 |
|---|---|---|---|
| `EnableMSU` | `true` / `1` / `false` / `0` / `deluxe` / `opuz` / `deluxe-opuz` | `false` | 是否启用 MSU，以及变体 |
| `MSUPath` | 路径前缀（直接拼编号） | `msu/alttp_msu-` | 音轨路径前缀，相对工作目录 |
| `MSUVolume` | `0%` ~ `100%` | `100%` | MSU 混音音量 |
| `ResumeMSU` | `0` / `1` | `0` | 离开又回到大地图时，续播还是从头播放 |

需要知道的四个行为：

1. **`EnableMSU` 只被自动检测"补开"，不会被强制覆盖。** 程序读 ini 之后做一次
   探测：如果 `EnableMSU` 是关闭状态（`0`/`false`）而探测到了音轨文件，就自动
   设为普通 PCM（`1`）；如果 ini 里已经写明 `deluxe`，这个值会被保留。
   反过来，手写 `EnableMSU = true` 但一个音轨文件都没有时，启动仍按 MSU 处理
   （输出 44100 Hz），每首曲子会打开失败并回退原版 SPC 音乐，
   日志里能看到 `MSU: track N not found`。
2. **`opuz` 变体在 3DS 上会被丢弃。** Opuz 要求 48000 Hz 输出，而本移植版在有
   MSU 时固定 44100 Hz，所以启动时会把 `opuz` 标志清掉（保留普通 PCM / deluxe）
   并写一条日志。3DS 上请使用 `.pcm` 音轨。
3. **`MSUPath`、`MSUVolume`、`ResumeMSU` 手写有效**，检测不会改动它们。
4. **检测只看四个固定位置**：`msu/1.pcm`、`msu/0.pcm`、`msu1.pcm`、`1.pcm`。
   如果你把音轨放在别的目录（例如 `MSUPath = music/`），自动开启不会命中，
   需要手写 `EnableMSU = true` 才会尝试加载。

最小可用配置：

```ini
[Sound]
MSUPath = msu/alttp_msu-
```

用纯数字命名（`msu/1.pcm`）时写 `MSUPath = msu/`。

`AudioFreq` / `AudioChannels` 在本移植版中由程序按机型与 MSU 状态自动设定，
写在 ini 里不会生效，保留只是为了与其他平台的 `zelda3.ini` 保持一致。

> **关于 Opus 变体**：`EnableMSU = opuz` 在通用配置解析里是合法值，但它要求
> 48000 Hz 输出，而本移植版在有 MSU 时固定使用 44100 Hz（见第 6 节），
> 因此 3DS 上**请使用 PCM 音轨**，不要用 `.opuz`。

---

## 6. 采样率与性能

程序在启动时按以下规则决定输出采样率（判定依据是 **MSU 是否启用**，
即自动检测到的结果或 ini 里显式写下的值）：

| 机型 | MSU 未启用 | MSU 已启用 |
|---|---|---|
| Old 3DS | 32000 Hz | 44100 Hz |
| New 3DS | 44100 Hz | 44100 Hz |

为什么不是一律 44100：

- MSU-1 PCM 是 44100 Hz，而混音器**不做重采样**，所以启用 MSU 时必须 44100，
  否则会变速变调。
- SPC700 解释器的工作量固定为每帧 534 个采样，与输出采样率无关；提高输出
  采样率只增加 DSP 重采样这一步（534 → 735 个输出采样，约 +38%）。
- Old 3DS 余量最小，所以 MSU 关闭时保持 32000 Hz；New 3DS 余量足够，
  一律 44100 Hz，重采样比更"整"。
- MSU 真正播放时，音乐直接来自 PCM，DSP 合成与重采样那一步被跳过
  （SPC700 状态仍继续推进，音效与计时器依赖它）。

实际听感：Old 3DS 上启用 MSU 后音乐音质明显提升；新增负载主要是 SD 读取，
而被跳过的 DSP 合成本身也是原先最贵的一环。

---

## 7. 故障排查

先看 `sdmc:/3ds/Zelda 3DS/runtime.log`，搜索 `MSU` 和 `Audio config`。
这三行基本能定位大部分问题：

| 日志 | 含义 |
|---|---|
| `Audio config: ... MSU on ... (enable=0x01, tracks found)` | 已启用，采样率切到 44100 |
| `Audio config: ... MSU off ... (enable=0x00, tracks not found)` | 未启用，仍用原版音乐 |
| `Audio: dropping Opuz flag ...` | ini 里的 `opuz` 被清除，按 PCM 播放 |
| `MSU: loading track N from <路径>` | 正在打开第 N 首，`<路径>` 就是实际拼出的位置 |
| `MSU: track N not found at <路径> (falling back to SPC)` | 第 N 首打不开，已回退原版音乐 |

> 注意：为减少 SD 卡写入，MSU 相关日志**每次启动只记录前 24 条**，
> 之后的切曲不会再有新行。

### 7.1 没有声音 / 还是原版音乐

按顺序排查：

1. **文件在探测列表的某个位置吗？** 程序依次尝试
   `msu/alttp_msu-1.pcm`、`msu/1.pcm`、`alttp_msu-1.pcm`、`1.pcm`。
   多套一层目录（`msu/alttp/1.pcm`）检测不到。
2. **文件名匹配前缀吗？** `alttp_msu-01.pcm`（补零）、`1.PCM`（大写）、
   `Track 1.pcm` 都不会被加载。
3. **`MSUPath` 和你的命名对得上吗？** 它直接拼在编号前面，不补任何分隔符：
   默认 `msu/alttp_msu-` 读 `msu/alttp_msu-1.pcm`；纯数字命名要写 `msu/`。
4. **是不是老 profile？** profile 的 `zelda3.ini` 会覆盖根目录那份。
   老 profile 里没有 `MSUPath` 时，检测到音轨后会自动把探测到的前缀填进去，
   所以通常无需手动处理。若你手写过错误的 `MSUPath`，删掉那一行即可让
   自动探测接管。
5. **只有某一首没音乐？** 那个编号的文件缺失或损坏，属于正常回退。
   用第 3.2 节的脚本确认。
6. **`dspfirm.cdc` 在吗？** 缺失时游戏根本启动不到这一步，会有明确提示。

### 7.2 播放速度变慢 / 音调不对

典型的采样率不匹配。MSU 播放时输出固定 44100 Hz 且不做重采样：

- 音轨本身是 48000 Hz（例如被转码过）→ 会偏慢、偏低。
- 音轨是 32000 Hz 或单声道 → 会偏快、偏尖，或声道错乱。

处理：用音频工具按 **44100 Hz / 16-bit / 立体声** 重新导出，并确认文件头仍是
`MSU1` + 4 字节 `repeat_position`。校验脚本能查出结构与对齐问题，但**查不出
真实采样率**，这一步得靠自己确认。

### 7.3 切曲卡顿

MSU 是边玩边从 SD 卡读的，切曲瞬间会有一次文件打开和缓冲填充：

- 换用速度更好的 SD 卡（Class 10 / A1 以上），避免劣质卡套。
- 确认 SD 卡没有同时被其他程序大量读写。
- 只保留常听的曲目，减少目录项数量。
- 卡顿只发生在切曲那一下，属于预期；持续卡顿更可能是渲染负载，
  请按 `platform/3ds/README.md` 的诊断流程抓 `L + R + A` 转储。

### 7.4 日志在哪看

`runtime.log` 位于 `sdmc:/3ds/Zelda 3DS/runtime.log`：

- 每次启动重建，只反映最近一次运行。
- 直接用文本编辑器打开，搜索 `MSU`。
- 若用它求助，请连同 `zelda3.ini`（含 profile 里那份）一起附上。

---

## 8. 常见问题 FAQ

**Q：发布包里为什么没有音轨？**
版权原因。音轨是第三方编曲/录音作品，不能随程序分发，需要你自行准备。

**Q：需要多大空间？**
44100 Hz × 4 字节 ≈ 每分钟 10 MB。按整套曲目的总时长估算，
例如 100 分钟约 1 GB。只放常用的十几首会小很多。

**Q：能只放一部分吗？**
可以。缺的编号回退原版音乐，已经在播的曲目不受影响。

**Q：换了 ROM 之后 MSU 怎么配？**
每个 ROM profile 有自己的 `zelda3.ini`。新 profile 会从内置配置复制，
默认已带 `MSUPath = msu/alttp_msu-`；老 profile 会自动补齐前缀。

**Q：可以在游戏里开关 MSU 吗？**
不能。下屏设置只有画面、加速倍率、开发者选项、更新、重启。
开关 MSU 的方式是放/删 `msu/` 里的文件，然后重启游戏。

**Q：`EnableMSU` 我写了 `true`，为什么日志还是 `MSU off`？**
因为 `MSU on` 需要音轨真的存在：探测路径没有文件时，启用也加载不出音轨，
启动阶段就不会把 MSU 标记为开启。请先确认 7.1 的第 1–3 条，再检查
`MSUPath` 前缀拼出来的路径里确实有对应编号的文件。

**Q：`deluxe` 和 `opuz` 能用吗？**
`deluxe` 是 MSU Deluxe 变体，需要对应的音轨包；手写的 `EnableMSU = deluxe`
会被保留（启动检测只补开关闭状态，不覆盖它）。`opuz` 需要 48000 Hz 输出，
与本移植版固定的 44100 Hz 冲突，启动时该标志会被清除并记一条日志，
所以 3DS 上**请用 `.pcm` 音轨**。

**Q：用了 MSU 会更耗电 / 掉帧吗？**
音轨解码与 SD 读取会多一点负载，但音乐不再经过 DSP 合成，总体接近。
Old 3DS 在复杂场景若掉帧，可减少曲目数量。

**Q：音效听起来还是老的，正常吗？**
正常，音效不由 MSU 音轨提供，见第 1 节。

---

## 相关文档

- [`platform/3ds/README.md`](../platform/3ds/README.md)：安装、显示、诊断与构建
- [`tools/check_msu_tracks.py`](../tools/check_msu_tracks.py)：音轨校验脚本
- [`docs/old3ds-pica200-design.md`](old3ds-pica200-design.md)：Old 3DS 渲染实现记录
