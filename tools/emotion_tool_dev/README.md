# emotion_builder（PC 表情包生成）

面向固件 **M1 layered**：静态情绪底图 + 独立嘴/眼小图 + 可选 life 轨。  
**不重编码**输入 MJPEG；只建索引、分段、ROI、RGB565、校验。

源码目录：`emotion_tool_dev/`。GitHub 镜像：`tools/emotion_builder/`。

## 推荐发布包

| 包 | 本地路径 | 角色 |
|----|----------|------|
| **v5 activity feather** | `mjpeg_ai_dialogue_v5_activity_feather` | 当前上板回退基线：canonical 活动岛嘴层 + 双 48 行 life 候选 |
| v5p2 semantic life | `mjpeg_ai_dialogue_v5p2_semantic_life` | 离线候选：happy 只批准下方身体/衣饰微动，拒绝上方头发轨 |
| v5p4 mouth-focus soft-seam | `mjpeg_ai_dialogue_v5p4_mouth_focus_soft_seam` | 离线候选：v5p3 头发语义门 + 四个非强情绪 medium/large 软接缝；夜间长泡期间不部署 |
| v2 canonical | `mjpeg_ai_dialogue_v2_canonical` | 无 life 的基线 layered 包 |
| posebank | `mjpeg_ai_dialogue_v2_posebank` | **实验**，眨眼已回退，勿当正式包 |
| 旧 v2 / v3 | `mjpeg_ai_dialogue_v2` / `_v3_life_v1` | 归档参考，勿再发布 |

设备部署路径必须是 `/sdcard/dialogue_v2/`（整包拷贝）。

## 用法

```bat
run_dialogue_v2.bat
```

```powershell
pip install -r requirements.txt
python dialogue_emotion_builder.py --input "C:\bake\mjpeg_ai" --output "C:\bake\mjpeg_ai_dialogue_v4_temporal_feather"
```

全画幅 AI 素材常被判 `layered_safe=false`；强制 layered 须肉眼查 `mouth_roi_preview.png`：

```powershell
python dialogue_emotion_builder.py --input "C:\bake\mjpeg_ai" --output "C:\bake\out" --force-layered
```

`--profile` 可覆盖自动 ROI/分段；改完必须重校验。

## P1 多 IP Character Pack v3 编译层

旧 builder 继续生成固件当前可读的 v2 包；不要为了 v3 改写或重编码这条已验路径。外围编译器只读 v2 媒体，根据角色 profile 选择该角色真正拥有的嘴、眼、身体 life 和短过渡能力，生成 v3 sidecar 与覆盖/接缝/语义/资源报告：

```powershell
python character_pack_compiler.py `
  --v2-pack "C:\bake\xiaozhi-p4-epdainaozhong0109\mjpeg_ai_dialogue_v5p3_mouth_focus" `
  --character-profile "C:\bake\xiaozhi-p4-epdainaozhong0109\emotion_tool_dev\profiles\current_ip_v1.json" `
  --output "C:\bake\out\character_pack_v3.json" `
  --report "C:\bake\out\character_compile_report.json"
```

- `profiles/current_ip_v1.json`：当前人物 IP；只批准已验嘴层与 standby/happy 下半部 track1，强情绪静态降级。
- `profiles/robot_no_mouth_v1.json`：结构测试 profile；没有嘴和眼时不读取 ROI/嘴眼资产，证明六 canonical 不等于六套固定人体坐标。
- profile 的画布必须与素材一致；不同 IP 必须使用自己的源素材，不能把人物底图冒充机器人包。
- `approved_layers` 与 `approved_life_tracks` 是人工发布门；自动检测结果不能自行升级为正式能力。
- 编译器只读取并校验 `frames.mjpeg` SHA-256，不调用 JPEG 编码器；v2 `pack_manifest.json` 和媒体文件保持不变。

离线测试：

```powershell
$env:PYTHONDONTWRITEBYTECODE=1
python -m unittest discover -s emotion_tool_dev -p "test_character_pack_compiler.py" -v
```

life 的运动分数只负责找候选，正式素材应使用语义批准门。例如只保留 happy 的自动候选 track 1：

```powershell
python dialogue_emotion_builder.py --input "C:\bake\xiaozhi-p4-epdainaozhong0109\release_source_v5_exact" --output "C:\bake\xiaozhi-p4-epdainaozhong0109\mjpeg_ai_dialogue_v5p2_semantic_life" --profile "C:\bake\xiaozhi-p4-epdainaozhong0109\mjpeg_ai_dialogue_v5_activity_feather\profile.generated.json" --force-layered --skip-strong-hub --skip-release-layer --life-approved-tracks happy:1
```

`profile.generated.json` 会保存 `life_track_policy`。不同素材的 track id 不具备通用语义，必须重新看 `life_preview.gif` 后批准；禁止在固件里硬编码“永远关闭 track 0”。

P-seam 候选只扩大四个非强情绪的 medium/large 活动岛软过渡，不改 MJPEG、ROI 或固件协议：

```powershell
python dialogue_emotion_builder.py --input "C:\bake\xiaozhi-p4-epdainaozhong0109\release_source_v5_exact" --output "C:\bake\xiaozhi-p4-epdainaozhong0109\mjpeg_ai_dialogue_v5p1_p_seam_candidate" --profile "C:\bake\xiaozhi-p4-epdainaozhong0109\mjpeg_ai_dialogue_v5_activity_feather\profile.generated.json" --force-layered --max-mouth-soften 1.5 --skip-strong-hub --skip-release-layer
```

该输出默认仅供离线查看 `mouth_roi_preview*.png` 与 `mouth_seam_heatmap.png`；未完成人工观感门前不要复制到 SD。

合并当前 v5p3 的头发语义门与 P-seam 时，两项批准必须显式写出，避免重新放出 standby/happy 的上方头发轨：

```powershell
python dialogue_emotion_builder.py --input "C:\bake\xiaozhi-p4-epdainaozhong0109\release_source_v5_exact" --output "C:\bake\xiaozhi-p4-epdainaozhong0109\mjpeg_ai_dialogue_v5p4_mouth_focus_soft_seam" --profile "C:\bake\xiaozhi-p4-epdainaozhong0109\mjpeg_ai_dialogue_v5_activity_feather\profile.generated.json" --force-layered --max-mouth-soften 1.5 --skip-strong-hub --skip-release-layer --life-approved-tracks standby:1 --life-approved-tracks happy:1
```

## 输出要点

- `pack_manifest.json`：六表情入口与 render 能力
- 每情绪：`frames.mjpeg`（原样拷贝）+ `manifest.json` + `mouth/` + `eye/` RGB565
- v4：`life/track_0`、`life/track_1`（各 ≤48 行，预合成到 canonical hold base）
- `analysis_report.csv` / `validation_report.json`
- `contract_report.json`：按固件真实加载边界检查六表情入口、安全相对路径、ROI、RGB565/A8尺寸与哈希、48行/单层预算及life语义批准状态

已有素材包无需重建即可做只读契约检查：

```powershell
python dialogue_pack_contract.py "C:\bake\mjpeg_ai_dialogue_v5p4_mouth_focus_soft_seam"
```

`errors` 非空时禁止复制到 SD；`auto motion track is not semantically approved` 是体验警告，须看对应 `life_preview.gif` 后用 `--life-approved-tracks` 明确批准或留空禁用。

固件契约：每 tick **最多一条** life 或嘴带；latest/drop-old；**禁止**再解 480×480 抠嘴；life 直接覆盖 ROI，**禁止**与上一帧累计 alpha。

冷启关键字：`!!FACE_S1CR h=1 pack=1` → 说话期 `mouth_arm` / `mouth_blit`。

## 与旧工具的关系

`emotion_roi_builder.py` 是更早的「自动找统一 ROI」脚本，**不能**替代本 builder。  
工作区 `tools/emotion_roi_builder/` 若只剩 `__pycache__`，视为死目录，勿再发布。
