# G4-3 Decoder ESP32-P4 生产适配映射

## 1. 裁决边界

本文件把生产 `mjpeg_decoder_stage` 与
`emotion_video_player.c::hw_decode_frame_optimized`逐项对表。s1gh 已把 Decoder 接入生产，
`components.decoder`指向生产纯C stage，ESP资源同步由player adapter承担；
板上为s1gh，生产迁移为3/5。

候选只拥有：输入buffer准备与拷贝、输出buffer准备、decoder lazy-open、同步process、
一次 `BUF_NOT_ENOUGH` 扩容重试，以及解码结果/耗时返回。以下职责永久留在旧wrapper，
直到各自独立阶段：FrameSource claim与cache mutex、播放deadline/EOF/error counter、
frame callback、WDT breadcrumb、`WdtContendNoteMjpegFrame`、PresentSink与LVGL/DMA。

## 2. Backend逐项映射

| Candidate port / state | ESP生产真源 | 守恒要求 |
|---|---|---|
| `get_free_psram` | `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` | 低于`required+512KiB`直接`NO_MEM`，不得先释放旧input/output |
| `now_us` | `esp_timer_get_time()` | `memcpy_ms`只包住copy；`decode_ms`从第一次process前到可回调输出就绪，callback不计入 |
| `alloc_input` | `heap_caps_aligned_alloc(max(in_frame_align,64), required, MALLOC_CAP_SPIRAM)` | 最小64KiB；旧input先free；失败时按历史语义清空output |
| `free_input` | `heap_caps_free` | 不改变FrameSource/cache所有权 |
| `alloc_output` | `esp_video_codec_align_alloc(out_frame_align, needed, &actual)` | 初始`canvas_w*canvas_h*2`；扩容用frame-info image size |
| `free_output` | `esp_video_codec_free` | frame-info失败发生在free之前；扩容alloc失败后pointer为空 |
| `open_decoder` | `esp_video_dec_open(&hw_dec_cfg, &hw_dec_handle)` | lazy-open；失败映射`ESP_FAIL`，本批不增加close/reopen策略 |
| `process` | 构造`esp_video_dec_in_frame_t/out_frame_t`后`esp_video_dec_process` | `pts=dts=current_frame*(1000/frame_rate)`、`consumed=0`；只允许两个静态调用点 |
| `get_frame_info` | `esp_video_dec_get_frame_info` | 首次扩容失败为`ESP_FAIL`；成功解码后宽高为0时查询失败仍按历史语义忽略 |
| `get_image_size` | `esp_video_codec_get_image_size(output_format,&res)` | 不自行改变format、stride或像素布局 |

结果映射固定为：`OK→ESP_OK`、`INVALID_SIZE→ESP_ERR_INVALID_SIZE`、
`NO_MEM→ESP_ERR_NO_MEM`、其余candidate错误→`ESP_FAIL`。Frame claim产生的
`ESP_ERR_TIMEOUT/INVALID_STATE/NOT_FOUND`仍在Decoder边界之前返回，不由候选重新解释。

## 3. 生产接线形态（独立marker）

1. 新生产文件使用与候选相同的纯C核心；板级ESP backend只实现port，不包含播放或显示。
2. `hw_decode_frame_optimized`在现有claim与mutex释放之后构造局部resources/request；调用后
   无论成功失败都把input/output/handle/frame-info同步回player，避免失败路径丢所有权。
3. 编译期开关 `EMOTION_VIDEO_USE_DECODER_STAGE` 保留完整旧解码体为R0；默认flip只能在
   Host、P4对象探针、同图release与镜像门通过后进行。
4. candidate成功后，旧wrapper继续执行原frame callback与breadcrumb，并以候选返回的
   `memcpy_ms/decode_ms`加原`cb_ms`调用`WdtContendNoteMjpegFrame`。
5. 本marker不改FrameSource游标、播放帧率、EOF/第11次错误、回调抑制、缓存锁、任务栈、
   PSRAM频率、输出format或任何可见动画参数。

## 4. M+/M−与回退

- H：适配后同一帧计划的status、资源状态、process次数、present输入和三段耗时语义守恒。
- M+：Host资源契约与production source guard全绿；P4对象无LVGL/RTOS依赖；release镜像门
  通过；exact新marker、SD/seed、3–5分钟AFE健康；风险匹配V2会话无回退。
- M−：第二次以上process、output双free/悬挂、callback/WDT note丢失、锁范围变化、
  present像素/尺寸变化、音频或会话异常、启动风暴/PANIC/WDT。
- R0：运行源码编译期开关回旧体；R1：OTA1 s1fw。任何M−立即停止G4-3，不在失败候选上
  继续叠PlaybackClock或PresentSink。

## 5. s1gh 实际结果与证据边界

- Host直接编译生产stage，10组资源/失败同步契约PASS；strangler=`migrated=3/5`。
- 同图release与P4镜像门PASS：7段、pre-IROM padding=`0x28`；BIN/ELF已独立归档。
- 首次release因32字节DROM增长跨64KiB边界被镜像门拒绝；把40字节只读backend dispatch表
  放入内部DRAM后恢复合法布局，没有删除功能或放宽门槛。
- 受控USB启动在1199ms命中exact s1gh、61464ms命中delayed marker；SD、六索引、seed 6/6、
  AFE连续，100秒内WDT/PANIC均为0。此证据只记V1，不冒充真实会话V2或常规6–12h V3。
