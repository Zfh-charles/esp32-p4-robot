# 小智 P4 代码治理与架构演进

## 1. 治理目标

治理不是追求“文件越短越好”，也不是重新设计全部固件。目标是降低三种真实成本：

1. 修改一个产品行为时，需要同时理解的模块数量；
2. 修改后必须上板才能发现错误的比例；
3. 显示、音频、网络和板级机制互相污染后造成的回归范围。

当前 `main` 约 8.55 万行、453 个 C/C++ 文件，但债务高度集中：

| 热点 | 行数（2026-08-23） | 混合职责 |
|------|------------------:|----------|
| `eezui_display_adapter.cc` | 4438 | LVGL、字幕、表情策略、资源加载、显示事务、动画调度、诊断 |
| `application.cc` | 2823 | 会话、音频策略、协议、提醒、OTA、板级表情策略、诊断 |
| `emotion_video_player.c` | 2674 | MJPEG 索引、读取、解码、播放状态、显示提交、生命周期 |

三者约占 `main` 的 11.6%。治理优先降低这三个热点的变化耦合，不批量移动其余稳定板级代码。

## 2. 第一性架构

依赖只能从外向内：

```text
Drivers / Board / ESP-IDF / LVGL / modem / SD
                    ↓ implements
Adapters: display, audio, protocol, storage gateways
                    ↓ implements ports
Use cases: Conversation, Reminder, Visual Coordination
                    ↓
Domain policies: session transition, emotion intent, budgets
```

- **Domain policy**：纯数据与纯决策；不得 include ESP-IDF、FreeRTOS、LVGL、具体板型或协议实现。
- **Use case**：编排业务步骤，只依赖抽象 port 与 domain；不直接解 JPEG、操作 GPIO、调用 LVGL。
- **Adapter**：把 port 翻译为 MQTT/UDP、AudioService、Display 等现有接口。
- **Driver/board**：唯一允许知道 ML307、SD/MMC、DMA、LVGL canvas 和 P4 特殊约束的外层。
- **Application**：最终退化为 composition root + event dispatch，不继续吸收产品策略和板级细节。

资源约束是本项目的架构组成部分，不是“性能优化附件”。视觉、音频、网络和未来传感器通过 `ResourceClaim` 描述代价，由准入层裁决；业务策略不得猜测某个硬件对象是否繁忙。

## 3. 三本书在本项目中的可执行解释

### 《架构整洁之道》

- 业务规则不依赖框架细节；板级实现依赖业务定义的 port。
- `application.cc` 直接 include `boards/ep-chat-p4-ml307/**` 是现存债务，先冻结数量，再用 facade/port 绞杀。
- 可测试性不是额外工作：无法脱离设备运行的策略，说明策略与机制尚未分离。

### 《代码整洁之道》

- 命名表达产品语义：`EndSessionAndRestoreIdle` 优于多个布尔量组合。
- 函数只处于一个抽象层级；“判断预算”和“提交 LVGL”不得在同一函数完成。
- 注释解释硬件约束和决策理由，不复述代码；实验 marker 和历史现场不进入长期业务接口。
- 布尔参数超过一个时优先改为命名请求对象或策略枚举。

### 《重构：改善既有代码设计》

- 先建立特征测试，再移动代码；每步行为守恒。
- 使用 Branch by Abstraction / Strangler：旧路径旁新增 port 和实现，R0 切换，稳定后删除旧路径。
- 重构与功能变化分批；调用图、所有权或时序变化必须独立 marker。
- 不以“大重写后更干净”为验收，验收是认知成本下降且设备行为不变。

## 4. 治理棘轮

`tools/code_health/architecture_guard.py` 以当前债务为上限：

- 23 个超过 500 行的历史文件不得继续增长；减少是绿灯，增加是失败。
- 新 C/C++ 文件默认不得超过 500 行。
- 三大热点总行数不得超过 9935；拆分时必须让总量不增加。
- `application.cc/.h` 中 ML307 条件编译数量冻结为 23/3。
- `application.cc` 现有 4 个板级 include 只作为债务白名单，不得新增。
- 新建 `main/domain/**` 或 `main/use_cases/**` 后，框架/板级 include 会直接失败。

棘轮只防止恶化，不假装当前结构已经合格。降低基线必须通过代码评审手工修改 baseline；禁止自动“接受新基线”。

## 5. 演进顺序

### G0：护栏（本批）

- 架构基线、自动检查、自测、治理说明。
- 不改运行代码，不占真机稳定性门。

### G1：抽纯策略（低风险）

依次抽取并在 PC 上做特征测试：

1. emotion canonical/文本意图映射；
2. VisualBudget 与 VisualCommit 准入决策；
3. 会话退出、空闲恢复与 reminder busy 判定。

只移动纯函数和数据结构，不改变调用顺序、任务、锁、分配或日志关键字。

### G2：Application 瘦身

- 定义 `VisualPort`、`SessionAudioPort`、`ReminderPort`。
- 先由 legacy adapter 转发到当前实现，默认仍走旧路径。
- `Application` 不再直接 include ML307 具体显示头；板级对象在 composition root 注入。
- 每抽一项，`application.cc` 必须净减行且对应 host test 增加。

### G3：显示上帝类绞杀

按职责建立旁路组件，不原地重写：

- `FaceAssetRepository`：manifest/seed/patch 的读取与校验；
- `FaceStateReducer`：requested/pending/committed 纯状态；
- `VisualCommitScheduler`：预算、过期和准入；
- `LvglFaceBackend`：唯一 LVGL/flush 执行者；
- `CaptionPresenter`：字幕和 typewriter。

旧 `EezuiDisplayAdapter` 先作为 facade 转发。每个 flip 独立 R0、行为夹具和 marker。

### G4：MJPEG 播放器分解

按数据流拆为 index reader、frame source、decoder、playback clock、present sink。C ABI 暂时保留，内部逐步替换；禁止在抽取批次改变帧率、缓冲归属或 DMA 时序。

具体生产切分顺序、IndexReader 第一刀、镜像风险和 G5 准入见
[`docs/mjpeg-g4-strangler-plan.md`](mjpeg-g4-strangler-plan.md)。

### G5：删旧路径

只有新路径完成行为守恒、冷启/多轮会话和寿命门后才删除旧实现。删除量应大于新增胶水量；连续两个 marker 未过门则停止结构切换。

## 6. 每批治理完成定义

- 架构棘轮 PASS，且基线没有被向上调大；
- 对应 host 特征测试 PASS；
- 若改 `main/**`，同一构建图 `V0-RELEASE` PASS；
- 改所有权、任务、锁、启动顺序或冷启敏感链时，必须有 R0、V2-Cold 和寿命证据；仅增加 legacy facade、保持调用顺序与运行语义的 D0~D1 抽取，可按作战卡走 V2-Auto，但最多连续两个 marker，窗口满即做对应场景收口；
- 记录净增减行、核心层板级依赖数、触及文件数；
- 功能改动与结构重构不由同一个因果结论认领。

## 7. 衡量治理是否有效

每周看趋势，不追逐单次漂亮数字：

- 三大热点总行数只降不升；
- `Application` 板级条件编译与板级 include 逐步归零；
- 产品策略 host test 数增加；
- 同类体验改动触及文件数下降；
- 构建、冷启和多轮会话回归率不升；
- 删除的旧路径行数最终大于新增 facade/胶水行数。

## 8. 2026-08-26 治理冲刺裁决

- G0–G3 已完成并保持架构棘轮；当前扫描为480个一方C/C++文件、87455行、
  三热点9858行、findings=0。
- G4五个职责均已有生产组件和Host契约；运行时只准入IndexReader/FrameSource。
  Decoder与PlaybackClock各自出现真机稳定性M−，PresentSink只有Host证据。
- `mjpeg_runtime_selection.h`是唯一编译期选择真源，版本化manifest记录每个
  职责的选择与运行证据；guard必须同时满足组件齐备、配置同步、runtime选择和逐职责
  证据，才可能给出G5 ready。
- `s1gj`把运行语义回锚到已验的G4-2路径，同时保留全部生产组件供后续二分。
  这完成了治理流程的结构与门禁闭环，但不等于G5删除完成。
- G5当前正确结果是`blocked`而非删除：Decoder/Clock/Sink的运行证据未通过。
  赶工授权不允许伪造寿命证据，也不允许删除仍承担R0回退的legacy体。

### 2026-08-30 状态增量

- `s1gp`已使PresentSink production通过Host、发布镜像、真机全场景V2和用户定义1小时零fatal门；
  G4运行选择由2/5提升到3/5。
- G5当前阻塞项缩减为Decoder与PlaybackClock各自的历史真机FAIL；PresentSink不再是阻塞项。
- 因此`--require-g5-ready`继续失败是正确门禁结果。除非两项失败职责分别取得新的独立V2/V3证据，
  不删除legacy，也不把“结构5/5、运行3/5”改写成G5删除完成。

### 2026-08-30 Clock direct-binding增量

- `s1gq`把PlaybackClock编译期选择移出逐帧入口并直接绑定owner字段；生产Host 12项、
  64×256步差分、P4峰值栈48B、Release/V1与真机全场景V2均PASS。
- 运行选择现为4/5，但Clock仍需同marker一小时零自然fatal的V3阶段门；Decoder保持legacy/FAIL。
  因而`g5_ready`继续为0，legacy仍是必要回退体。

### 2026-08-30 V3反证

- `s1gq`于4135939ms自然HP WDT，Clock运行证据由PENDING改为FAIL；设备已R1回到OTA1 s1fw。
- Decoder首个direct外联候选虽过Host语义门，但P4入口栈128B>64B，未进入生产源码。
- 两项证据共同表明“组件边界齐备”不能靠增加热路径调用层换取。G5保持BLOCKED；若未来继续，
  只能测试完整caller的编译期内联形态，或正式接受部分legacy为硅片受限平台的永久实现。
