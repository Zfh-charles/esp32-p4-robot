# GitHub 协作与仓库管理

## 1. 初始化仓库（维护者首次）

在项目根目录（含 `CMakeLists.txt` 的 `esp32-p4-i2cpolling/`）：

```powershell
git init
git add .
git status   # 确认无 .env、sdkconfig、build/ 被纳入
git commit -m "Initial commit: EP Chat P4 ML307 reminder firmware and tools"
```

在 GitHub 创建 **Private** 仓库后：

```powershell
git remote add origin https://github.com/Zfh-charles/esp32-p4-i2cpolling.git
git branch -M main
git push -u origin main
```

或使用 GitHub CLI：

```powershell
gh repo create Zfh-charles/esp32-p4-i2cpolling --private --source=. --remote=origin --push
```

## 2. 新成员克隆

```powershell
git clone https://github.com/Zfh-charles/esp32-p4-i2cpolling.git
cd esp32-p4-i2cpolling
```

然后按 [team-onboarding.md](team-onboarding.md) 配置本地 `sdkconfig` 与 `.env`。首次编译若报错或白屏，见 [ep-chat-p4-build-onboarding.md](ep-chat-p4-build-onboarding.md)。

## 3. 不提交的文件（已在 .gitignore）

| 路径 | 原因 |
|------|------|
| `sdkconfig` | 含本机 poll_url、编译选项 |
| `build/` | 编译产物 |
| `managed_components/` | IDF 组件管理器下载 |
| `.env` | 密钥 |
| `tools/mcp-calculator/.env` | MCP token |
| `*.bin` | 固件二进制 |

## 4. 应提交的模板

| 文件 | 用途 |
|------|------|
| `sdkconfig.defaults` | 通用编译默认项 |
| `sdkconfig.defaults.reminder.example` | 轮询功能 menuconfig 参考 |
| [docs/ep-chat-p4-build-onboarding.md](ep-chat-p4-build-onboarding.md) | P4 编译排障、显示与 SD 卡说明 |
| `tools/mcp-calculator/.env.example` | MCP 环境变量模板 |
| `tools/inference-api-demo/.env.example` | API 环境变量模板 |

## 5. 分支建议

| 分支 | 用途 |
|------|------|
| `main` | 可烧录的稳定版本 |
| `dev/*` | 功能开发 |
| `fix/*` | 缺陷修复 |

合并前建议：本地 `idf.py build` 通过 + 至少一台板子串口验证 `poll_no_reminder` / 主动 TTS 各一条路径。

## 6. Token 泄露处理

若误提交 `.env` 或 MCP token：

1. 立即在小智控制台 **轮换 MCP token**
2. 使用 `git filter-repo` 或 BFG 从历史中清除（Private 仓库仍建议处理）
3. 强制推送前通知团队

## 7. 可选：GitHub Actions

当前仓库 **未内置 CI**（ESP-IDF 编译需专用 runner）。团队可后续添加 self-hosted runner 或在合并前本地编译。
