# 服务端归一化方案 v2 — 存档（契约 / 测试向量 / 信封结构）

> 用途：固化「固件 ↔ 服务端」之间的协议契约，服务端照此实现即可与现有固件对齐。
> 最后更新：2026-06-18（salt 拍板：**固件内置 raw salt，Option A**）
> 相关：`docs/handover-reminder-normalization.md`、`docs/reminder-mqtt-wake.md`

---

## 0. 决策记录（Decision Log）

| 项 | 决定 | 备注 |
|----|------|------|
| 持久化 | **JSON 文件 + 文件锁** | 已定 |
| HMAC 契约 | 见 §2（从固件实现固化） | 服务端 `derive_mqtt_password` 必须逐字对齐 |
| **salt 位置** | **固件内置 raw salt 纯派生（Option A）** | 改动最小；安全级别为「混淆级」，详见 §5 |

### 为何选 Option A（salt 放固件）
- wake 报文**不带内容**（仅门铃），密码被破后果是骚扰/DoS，非泄密 → 不值得上「认证下发」重型方案。
- 「服务端派生 + 运行时首启下发」只有在**下发接口本身有认证**时才更安全；否则联网即可批量领密码，比内置 salt 更糟。
- 固件 `DeriveMqttPassword()` 参考实现已就绪，选 A 固件零改动。
- **升级路径**：若日后要「镜像零机密」，走**烧录期注入**（刷机脚本按 MAC 调服务端派生、写 NVS），而非运行时开放接口。

---

## 1. 设备标识占位符

| 占位符 | 含义 | 示例 |
|--------|------|------|
| `{device_id}` | MAC 带冒号、小写 | `30:ed:a0:e1:b5:28` |
| `{mac}` | 同 `{device_id}` | `30:ed:a0:e1:b5:28` |
| `{mac_clean}` | MAC 去冒号、小写 | `30eda0e1b528` |

派生与 topic/username/client_id 一律以 `{mac_clean}` 为输入。

---

## 2. HMAC 密码派生契约

服务端 `derive_mqtt_password(mac)` 必须与固件 `DeriveMqttPassword()` 完全一致：

| # | 项 | 值 |
|---|----|----|
| 1 | 算法 | **HMAC-SHA256** |
| 2 | key | salt 的 **32 字节原始值**（`hex_decode(CONFIG_REMINDER_MQTT_WAKE_SECRET_SALT)`） |
| 3 | message | `mac_clean`（小写、去冒号），如 `30eda0e1b528` 的 ASCII 字节 |
| 4 | 输出编码 | **小写 hex**；固件当前取 HMAC 结果 **前 16 字节（32 hex）** 作为 MQTT 密码（见 `DeriveMqttPassword()`） |
| 5 | username | `esp32_{mac_clean}`，如 `esp32_30eda0e1b528` |
| 6 | client_id | `esp32-{mac_clean}`，如 `esp32-30eda0e1b528` |

> 固件实现位置：`main/reminder/reminder_mqtt_wake.cc` `DeriveMqttPassword()`（mbedtls `mbedtls_md_hmac` + `%02x` 拼接）。

### 2.1 参考实现（Python，服务端）

```python
import hmac, hashlib

def derive_mqtt_password(mac: str, salt: str) -> str:
    mac_clean = mac.replace(":", "").lower()
    return hmac.new(
        salt.encode("utf-8"),
        mac_clean.encode("utf-8"),
        hashlib.sha256,
    ).hexdigest()  # 64 hex chars, lowercase
```

### 2.2 测试向量（占位 salt `test-salt`）

> 下表用占位 salt `test-salt` 按 §2.1 计算所得，仅供服务端 `derive_mqtt_password` 单元对拍。
> 生产请换成真实 `CONFIG_REMINDER_MQTT_WAKE_SECRET_SALT`，并在固件侧用相同 salt 打印一次密码交叉校对。

| salt | mac | mac_clean | 期望 password（hex64） |
|------|-----|-----------|------------------------|
| `test-salt` | `30:ed:a0:e1:b5:28` | `30eda0e1b528` | `2963a2211f6621b2bdcbc95f12fb3c98bcdb0c225ba63e5e263de740bcc28940` |
| `test-salt` | `aa:bb:cc:dd:ee:ff` | `aabbccddeeff` | `87225ef651105418baa4bad2a98a3f542385bf7816d5f3801e5aa3cce7e97244` |

校对脚本（任意机器可跑）：

```bash
python -c "import hmac,hashlib; print(hmac.new(b'test-salt', b'30eda0e1b528', hashlib.sha256).hexdigest())"
```

---

## 3. 统一信封 v1（GET /pending 响应结构）

字段与固件解析逐一对齐（`ParsePendingResponse` + `ParseDeliveryMode`）。

### 3.1 无待提醒
```json
{ "has_reminder": false }
```

### 3.2 有待提醒（完整信封）
```json
{
  "has_reminder": true,
  "schema": "v1",
  "type": "reminder",
  "id": "meet-20260618-2200",
  "prompt": "10分钟后有产品评审会，请提前进入会议室。",
  "speak": true,
  "delivery_mode": "mcp_wake",
  "wake_text": "查提醒",
  "emotion": "neutral",
  "severity": "info",
  "expires_at": 1750255200
}
```

### 3.3 字段契约

| 字段 | 类型 | 必填 | 固件行为 |
|------|------|------|----------|
| `has_reminder` | bool | 是 | 非 `true` 即视为无提醒，直接返回 |
| `schema` | string | 否 | 非 `"v1"` 仅告警、仍尽力解析；缺省按旧格式兼容 |
| `id` | string | **是** | 缺失则解析失败；用于去重（NVS `last_id`） |
| `prompt` | string | **是** | 缺失则解析失败；播报文案 |
| `speak` | bool | 否 | 默认 `true`；`false` → `alert_only`（仅 UI/提示，不 TTS） |
| `delivery_mode` | string | 否 | `mcp_wake`（默认）/ `direct_wake` / `alert_only` |
| `wake_text` | string | 否 | `mcp_wake` 时发给云的 detect 短语；空则用 `CONFIG_REMINDER_WAKE_PHRASE` 或「查提醒」 |
| `emotion` | string | 否 | 播报前表情；默认 `neutral` |
| `severity` | string | 否 | 默认 `info`；非 `mcp_wake` 模式配合 `CONFIG_REMINDER_ANNOUNCE_REPEAT` 重诵 |
| `type` | string | 否 | 仅日志（`Envelope type=… severity=…`） |
| `expires_at` | int | 否 | 服务端队列过期用，固件当前不强制 |

### 3.4 delivery_mode 判定（与固件一致）
1. `speak == false` → `alert_only`
2. `delivery_mode == "direct_wake"` → `direct_wake`
3. `delivery_mode == "alert_only"` → `alert_only`
4. 其它/缺省 → `mcp_wake`

> 注：`direct_wake` 的 prompt 超过 32 字符时，固件会自动降级为 `mcp_wake`。

---

## 4. MQTT 唤醒报文（当前：门铃语义）

主题：`v1/notify/{mac_clean}`（与 `CONFIG_REMINDER_MQTT_WAKE_DEFAULT_TOPIC` 一致）

```json
{ "type": "reminder_wake", "device_id": "30:ed:a0:e1:b5:28" }
```

固件收到即 `TriggerPoll()` → 立即 GET `/pending`。**wake 不携带内容**，内容只通过信封 v1（§3）下发。

---

## 5. 安全说明（Option A 的边界）

- salt 烤进镜像：**dump 任意一台设备 flash 即可反推全部设备密码**（MAC 非秘密）。
- 当前定位为「**混淆级**」，与现状 `CONFIG_REMINDER_MQTT_TLS_INSECURE=y`、明文 8443 poll 的安全基线匹配。
- 加固选项（按需）：
  1. 开启 ESP32 **flash 加密 + secure boot**，让内置 salt 成为真正机密；
  2. 升级到「镜像零机密」时，用**烧录期注入**（刷机脚本算密码写 NVS），**不要**用运行时开放下发接口。

---

## 6. 落地顺序（P1→P1.5→P2→P3）

| 阶段 | 范围 | 状态 |
|------|------|------|
| P1 | 占位符 `{mac_clean}` + topic/username/client_id 模板化 | 固件已完成 |
| P1.5 | 服务端 JSON 文件持久化 + 文件锁 | 待出 diff |
| P2 | 信封 v1（schema/type/severity）+ 重诵 | 固件已完成；服务端按 §3 输出 |
| P3 | 派生密码（HMAC，§2）+ TLS 开关 | 固件已完成（salt=固件）；服务端实现 `derive_mqtt_password` |
