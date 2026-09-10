# Product Contracts（P0 离线契约）

这一目录把“角色素材是什么”和“设备能承担什么”从固件热路径中分离。当前只做离线描述与校验，不改变 `main/**`，也不代表这些未来设备已经存在。

## 四份契约

- `character-pack-v3`：六个 canonical 情绪、角色自身能力、已批准层、预合成不透明素材、哈希与资源声明。角色可以没有嘴或眼；这时用 `degraded/static_seed` 或角色自己的语义层表达，禁止伪造固定嘴部坐标。
- `device-capability-v1`：板卡、内存、共享总线/DMA/网络/电源域和可选外设。缺少摄像头、毫米波或推理器不会阻塞当前设备。
- `resource-claim-v1`：任务的截止期、WCET、内存、PSRAM burst、DMA、电源和降级链。实时音频不可丢；视觉、感知和推理必须能丢弃或降级。
- `visual-intent-commit-v1`：产品语义只发布 Intent；编排与预算通过后才生成 Commit。Commit 只引用 PC 预合成不透明资产，过期即丢、不补播。

`memory.internal_sram_bytes=0` 在规划样例中表示“尚未形成可承诺预算”，不是宣称芯片没有 SRAM。未来设备样例是结构演示，数值不作为 BOM 或性能承诺。

## 校验

```powershell
python tools/product_contracts/validate_contracts.py --self-test
python -m unittest discover -s tools/product_contracts -p "test_*.py" -v
```

校验器刻意无第三方依赖：JSON Schema 用于机器可读接口和后续生成器集成；当前 P0 的硬语义由 `validate_contracts.py` 与正反例测试执行。P1 才把 `emotion_tool_dev` 编译器接到 Character Pack v3；旧 v2 生成和固件读取路径暂不改变。

## 目录

- `schemas/`：四份版本化 JSON Schema。
- `examples/valid/`：当前人物 IP、无嘴机器人、当前 P4、未来传感器设备及典型资源声明。
- `examples/invalid/`：必须拒绝的缺失情绪、重复资源域、不可丢视觉、运行时 alpha。

任何契约升级都必须新增版本，不能静默改变既有版本含义。
