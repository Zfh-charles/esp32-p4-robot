# MJPEG G4 绞杀式拆分执行计划

## 1. 目标与边界

目标是把 `emotion_video_player.c` 按 `IndexReader → FrameSource → Decoder →
PlaybackClock → PresentSink` 分解，同时保留现有 C ABI、帧率、缓存归属、锁、
DMA、回调和错误恢复语义。G4 是可维护性治理，不是视觉功能升级；任何抽取批次
都不得顺带修改动画节奏或资源预算。

旧路径必须一直保留到新路径完成 V2/V3。每个生产 flip 单独 marker、单独回退，
并由 `tools/code_health/mjpeg_strangler_guard.py` 检查生产语义与 G5 准入。

## 2. 当前基线证据

- 基线固件：OTA0 `boot_trace_v10_s1ge_g3_caption_extract`；OTA1 保留 `s1fw`。
- G4a/G4b 已冻结索引、claim、decode、present、EOF、bypass 和错误计数语义。
- G4c 在真实生产源上检查 8 组有序锚点；当前阶段为 `legacy_guarded`。
- `build_frame_index` 在 s1ge map 中为 `.text=0x188`、`.rodata=0x6b`，只在
  standby 初载、异步加载和预载路径调用，不在逐帧显示热循环中运行。
- `main/CMakeLists.txt` 通过板目录 GLOB 收集 `.c/.cc`。新增生产文件必须做一次
  正常 CMake 重生成和同构建图 release；禁止手工替换对象后烧录。
- s1ge 当前 P4 镜像 padding 仅 `0x28`。任何新 TU 即使容量很小，也必须重新检查
  段顺序、TCM 单段与 pre-IROM padding，不能只看 BIN 大小或 hash。

## 3. G4-1：IndexReader（第一生产刀）

### 3.1 为什么先做它

它没有 JPEG 解码器句柄、输出 buffer、frame callback、播放 deadline 或 LVGL/DMA
所有权，是五个职责中硬件时序风险最低的一项。失败最多影响索引建立和素材可用性，
不会在运行期逐帧制造新的 PSRAM 突发。

### 3.2 预定文件与接口

- `main/boards/ep-chat-p4-ml307/mjpeg_index_reader.h`
- `main/boards/ep-chat-p4-ml307/mjpeg_index_reader.c`

接口保持无堆、无锁、无 FreeRTOS 依赖：调用方提供 MJPEG buffer、输出 entry 数组、
容量和可选 yield hook。IndexReader 只扫描并返回帧数；`video_cache_t.ready`、
`frame_count`、`index_built`、日志和 cache mutex 继续由 legacy wrapper 拥有。

必须守恒的细节：

1. SOI/EOI 取第一次闭合区间；候选小于 100B 或大于 2MiB 时仍消费整个候选；
2. 截断尾停止扫描并保留已建立条目；上限严格为 300；
3. 每消费 8 个候选执行一次调用方 yield hook，不能改成“每 8 个合法帧”；
4. 零合法帧仍返回成功，由 wrapper 写 `frame_count=0/index_built=true`；
5. entry 继续保持两个 `size_t`，用 C `_Static_assert` 守住大小与对齐。

### 3.3 绞杀与回退

旧 `build_frame_index` wrapper、四个调用点和所有日志文字保留。编译期开关
`EMOTION_VIDEO_USE_INDEX_READER` 默认开新实现；关闭时编回原内联扫描体，形成 R0。
真机异常时优先关闭该开关；启动风暴或无法取得 marker 时直接切 OTA1 `s1fw`（R1）。

### 3.4 验证门

- V0/T1：把 G4a 的 9 个 oracle 用例对照生产 IndexReader；补 yield 候选计数、
  零帧成功、截断尾和 300 帧帽反例。
- V0-Release：同一 CMake/Ninja 图构建，P4 image layout、分区余量、BIN/ELF hash，
  新旧路径符号与调用点唯一。
- V1：只写 OTA0+otadata，OTA1 不动；exact marker、无启动风暴、AFE/内存短健康。
- V2-Runtime：SD/seed 6/6、六表情加载、多轮对话、长回答、退出和第二轮唤醒。
- V3：与 s1ge/s1cm 口径一致；未形成完成寿命样本时只记零故障灰度时长。

G4-1 的硬前置是 s1ge baseline session V2。没有两轮真实会话日志时，只允许准备
测试和设计，不修改生产播放器。

## 4. 后续阶梯

| 阶段 | 唯一职责变量 | 主要风险 | 进入下一阶条件 |
|------|--------------|----------|----------------|
| G4-1 | IndexReader | 索引/加载、镜像段 | V2 + 滚动寿命无回退 |
| G4-2 | FrameSource claim/reset/seek | 游标推进、锁与坏 entry | G4a 对照 + V2 |
| G4-3 | Decoder | buffer、一次 resize retry、硬解句柄 | G4b 对照 + V2/V3 |
| G4-4 | PlaybackClock | deadline、EOF、连续错误 | FakeClock + 长回答/循环播放 |
| G4-5 | PresentSink | callback、显示/DMA 所有权 | 独占 marker + HIL/寿命门 |

Decoder 与 PresentSink 不同 marker；PresentSink 不与任何可见动画特性同批。连续两个
marker 未过行为或寿命门，停止热路径结构切换，保留已交付能力核。

### 4.1 G4-2 FrameSource 的两步准入

第一步只在`tools/validation/candidates/`建立未接线候选组件和 Host 差分夹具，禁止
把未接线`.c`放入板目录GLOB、让同一marker暗中对应不同镜像。组件只拥有
`view + cursor → claim`的
纯语义：成功claim在decode前推进；EOF和坏entry不推进；seek覆盖旧游标并按frame_count
取模。cache mutex、cache lifetime、JPEG输入复制、decoder、present和播放时钟仍归旧
wrapper。候选文件存在不等于生产迁移，manifest的`frame_source`在播放器实际调用前
必须保持`null`。

第二步才允许单marker接线：前置为候选与legacy差分、G4a oracle、code-health和架构门
全过；播放器只在原cache mutex范围内构造view并调用claim，错误码逐一映射，旧内联claim
由编译期开关保留R0。不得同批修复坏entry整数溢出、调整锁范围或抽取reset/seek调用点。
由于s1gf镜像padding仅`0x28`，接线前必须先做同图dry-run和完整P4布局门；候选阶段不
重生成构建图、不编译板级release、不消耗第二个自主marker。

**2026-08-26 落地状态**：第二步已由`s1gg`完成。生产只接`claim_next`，reset/seek、
decoder、present、clock和锁边界未动；Host差分与统一fast通过。同图release为
`7 segments / padding 0x20`，BIN/ELF已只读归档；OTA0启动1199ms exact marker、
`PANIC_LAST none`、六索引、seed 6/6和5分钟AFE健康通过，OTA1`s1fw`未动。G4 runtime
V2已由同代两轮会话、长TTS、退出/再唤醒和用户体验收口；V3仍累计中，因此不得把
2/5职责冒充完整production split。

### 4.2 G4-3 Decoder 的离线资源契约

生产接线前先在`tools/validation/candidates/`保留纯C候选，不进入板目录GLOB，也不改变
`components.decoder=null`。候选边界只拥有“已claim帧→同步硬解输出”的资源序列；
FrameSource游标、cache mutex、播放deadline、frame callback、WDT breadcrumb与PresentSink
仍由旧wrapper拥有。Host fake须锁定以下历史语义：

1. 输入不足时先检查`required+512KiB`空闲量；空闲不足不释放旧资源；
2. 输入重分配失败会清掉旧output，frame claim已经消费，不做补播；
3. output与decoder handle按需创建；`BUF_NOT_ENOUGH`只查询一次frame info、释放旧output、
   扩容并仅重试一次，第二次不足按普通失败；
4. frame-info失败发生在释放旧output之前；扩容分配失败后output为空；
5. `decoded_size=0`仍为成功但不产生可present像素；Decoder候选不调用任何显示回调；
6. 候选以C11`-Werror`编译并由C++ fake后端验证，防止只在主机C++语义下偶然可用。

当前离线候选与8组资源/解码契约PASS，manifest只记candidate而不计第三个production角色。
下一门须把真实ESP backend适配范围、错误码映射和旧新差分写成独立计划；在`s1gg` V3
或风险裁决前，不把候选移入`main/**`，不升marker、不构建烧录。

### 4.3 G4-4 PlaybackClock 的离线Effect契约

V3等待期可以并行准备纯时钟候选，但`components.playback_clock`仍为null，不允许越过
Decoder production次序接线。候选只接受显式`now_us`与decode结果，返回pause/warmup/
wait/decode、stall诊断、yield、stream-end和enter-error Effect；不得读取真实时钟、延时
任务、claim游标、调用解码器或显示。

Host反例锁定：resume覆盖旧deadline并执行300ms warmup+2s soft-start；resume初始间隔
至少200ms，而decode loop的历史软启动下限为100ms；成功帧从当前时间重新锚定、绝不补
赶旧deadline；每N帧只请求1tick Effect；EOF令下一循环立即due但cursor reset留在wrapper；
错误不移动deadline，第11次才进入error；1.5s无成功帧会重锚deadline并yield，stall日志按
1s限频。soft-start结束与stall同tick时，必须先恢复正常interval再执行stall门。

当前8组C11/C++ Host契约通过，candidate只计离线准备，不增加`migrated=2/5`。未来生产
flip必须独立于Decoder marker，并保留完整旧clock分支R0；若Decoder尚未过V2/V3，Clock
只能继续离线，不得为了缩短工期合并上板。

### 4.4 G4-5 PresentSink 的离线端口契约

离线候选只冻结当前`frame_cb`和WDT观测顺序，不实现LVGL、MIPI、DMA或panel driver。
`decoded_size=0`或callback为空时不得present，但仍须恰一次上报`memcpy/decode/cb=0`；
有callback时从callback前取时钟，分别在callback前后实时采样三次contend-window，不能把
三次结果缓存成一次。窗口活跃时breadcrumb顺序固定为`pre_frame_cb→post_frame_cb→
post_frame_note→yield→post_yield`；窗口状态中途变化时按每次真实采样决定对应Effect。

候选返回`presented/callback_ms`，frame像素指针、尺寸和user-data原样透传；它不拥有buffer
生命周期、解码资源、clock、cache、cursor或显示实现。当前6组Host反例通过，candidate
仅记录`production_wired=false`，不增加生产迁移数。PresentSink是G4中最高硬件风险项：
只有Decoder与Clock各自production marker完成V2/V3后才允许独占flip，并必须追加真实
LVGL/MIPI/DMA HIL与寿命门，Host PASS不得替代。

## 5. G5 删除准入

只有以下条件同时满足才允许删除 legacy 实现：五职责组件均存在；baseline session
V2、G4 runtime V2、G4 soak V3 均有可追溯证据；production guard 无语义漂移；
新路径默认启用且旧路径在一个完整 marker 周期未被回退；删除量大于新增胶水量。

删除批次仍需完整同图 release、P4 镜像门、冷启/多轮会话和寿命裁决。G5 完成前，
`emotion_video_player.c` 的历史函数存在是删除目标，不是已完成证明。

## 6. 2026-08-26 实际落点

生产组件已达到5/5，但`s1gk`的运行选择
`IndexReader=production / FrameSource=production / Decoder=legacy /
PlaybackClock=production / PresentSink=disabled`在3445743ms自然WDT并伴LoadAccessFault，
且同期发生第二轮无声/延迟出声回归，故Clock运行门FAIL。板上已自动切回OTA1 `s1fw`；
源码失败候选只保留复判，禁止误烧。

治理清单现为schema v2，同时记录`runtime_selection`与逐职责
`runtime_evidence`；manifest v3另记五个生产Host契约，guard输出
`dual_path_guarded split=1 contracts=1 runtime_sync=1 g5_ready=0`。Decoder与Clock连续两个
热路径职责失败后，PresentSink冻结，不再继续逐marker试撞。下一阶段先离线复判二者共有
控制态/时序边界并补可复现反例；未取得新证据和各自V2/V3前，G5删除命令必须继续失败。
