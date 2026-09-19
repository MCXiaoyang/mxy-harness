# mxy-harness
[![build](https://github.com/MCXiaoyang/mxy-harness/actions/workflows/build.yml/badge.svg)](https://github.com/MCXiaoyang/mxy-harness/actions/workflows/build.yml)
> 一个本地优先、协议无关的 LLM 中继守护进程。C++ 核心 + Unix Socket + Python/Flask 前端。
> 我没编译，linux和macos用户直接编译就行，Windows用户建议使用WSL进行编译并在WSL里运行

---

## 简介

`mxy-harness` 把任意 LLM API（OpenAI、Anthropic、Ollama、小厂自定义 HTTP）统一成一套协议，通过 Unix Domain Socket 暴露给前端。它不是又一个 ChatGPT 套壳，而是一个**可调试、可分支、可回放**的本地中继层。

---

## 特性

- **协议无关** — 内置 `openai` / `anthropic` / `ollama` / `custom` 四种 Provider
- **对话是树** — 任意消息可编辑、可重新生成、可从这里分支
- **本地优先** — 数据存 `~/.mxy-harness/`，socket 权限 `0600`
- **零第三方 C++ 依赖** — HTTPS 通过 `fork/exec curl` 实现，JSON 库自带
- **进程隔离** — C++ 守护进程崩溃不会带走 Flask UI
- **可中断** — 客户端断开 → 守护进程检测 EOF → `SIGTERM` 杀 curl

---

## 架构

```
┌──────────────────────────────────────────────┐
│  浏览器                                       │
│  WebSocket / SSE / HTTP                      │
└──────────────────┬───────────────────────────┘
                   │
┌──────────────────▼───────────────────────────┐
│  Python + Flask (app.py)                     │
└──────────────────┬───────────────────────────┘
                   │ Unix Domain Socket
┌──────────────────▼───────────────────────────┐
│  C++ 守护进程 (mxy-harnessd)                 │
│  - Session Engine  对话树 / 分支 / 编辑      │
│  - Provider        openai/anthropic/...      │
│  - HTTP Client     fork/exec curl            │
│  - Store           JSON 文件持久化            │
└──────────────────┬───────────────────────────┘
                   │ HTTPS
┌──────────────────▼───────────────────────────┐
│  OpenAI / Anthropic / Ollama / 任意 API      │
└──────────────────────────────────────────────┘
```

---

## 快速开始

### 依赖

- `g++ >= 8` (C++17)
- `make`
- `curl`
- `python3 >= 3.8`
- `flask`, `flask-sock`

### 安装 Python 依赖

```bash
cd python
pip install -r requirements.txt
```

### 编译

```bash
make
```

产物：`build/mxy-harnessd`

### 启动

终端 1 — 守护进程：

```bash
make daemon
```

终端 2 — Flask UI：

```bash
export MXY_HARNESS_SOCK="$XDG_RUNTIME_DIR/mxy-harness.sock"
cd python && python3 app.py
```

打开 <http://127.0.0.1:8080>

---

## socket 路径

守护进程默认：

- `$XDG_RUNTIME_DIR/mxy-harness.sock`
- 否则 `~/.mxy-harness/daemon.sock`

Flask 默认连 `/tmp/mxy-harness.sock`，启动前务必对齐：

```bash
export MXY_HARNESS_SOCK="$XDG_RUNTIME_DIR/mxy-harness.sock"
```

---

## 配置模型

UI 左下角 **⚙ 模型设置**。

| Provider | 用途 | 示例 |
|---|---|---|
| `openai` | OpenAI 兼容 | `https://api.deepseek.com/v1/chat/completions` |
| `anthropic` | Anthropic 官方 | `https://api.anthropic.com/v1/messages` |
| `ollama` | 本地模型 | `http://127.0.0.1:11434/api/chat` |
| `custom` | 任意 HTTP | 自定义模板 |

### custom Provider

- **请求体模板** — `{{model}}` `{{messages}}` `{{prompt}}` `{{system}}`
- **流模式** — `sse` / `ndjson` / `text` / `none`
- **流式路径** — 例 `choices.0.delta.content`
- **响应路径** — 例 `choices.0.message.content`

---

## 命令行

```
mxy-harnessd [options]

  --sock <path>   Unix socket 路径
  --home <dir>    数据目录
  --version       版本
  -h, --help      帮助
```

---

## 环境变量

| 变量 | 作用 |
|---|---|
| `MXY_HARNESS_HOME` | 数据目录 |
| `MXY_HARNESS_SOCK` | Flask 侧 socket 路径 |
| `XDG_RUNTIME_DIR` | 默认 socket 位置 |
| `PORT` | Flask 端口，默认 8080 |
| `HOST` | Flask 绑定地址，默认 127.0.0.1 |

---

## 数据目录

```
~/.mxy-harness/
├── config.json
├── conversations/
│   ├── conv_xxxx.json
│   └── ...
└── tmp/
```

---

## 协议

换行分隔 JSON。

**请求：**

```json
{"id":"req-1","cmd":"chat","params":{"conv_id":"conv_xxx","content":"hi"}}
```

**响应：**

```json
{"id":"req-1","type":"delta","text":"你"}
{"id":"req-1","type":"done","data":{"message_id":"m_xxx","content":"你好"}}
{"id":"req-1","type":"error","message":"HTTP 401"}
```

**命令表：**

| cmd | 说明 |
|---|---|
| `ping` | 存活探测 |
| `list_models` / `save_model` / `delete_model` | 模型配置 |
| `list_conversations` / `new_conversation` / `get_conversation` / `delete_conversation` / `rename_conversation` | 会话管理 |
| `chat` / `regenerate` | 生成 |
| `edit_message` / `delete_message` / `set_active_leaf` | 对话树 |
| `get_config` / `set_settings` | 设置 |

---

## 项目结构

```
mxy-harness/
├── Makefile
├── README.md
├── cpp/
│   ├── json.hpp
│   ├── util.hpp
│   ├── http.hpp
│   ├── store.hpp
│   ├── engine.hpp
│   ├── server.hpp
│   └── main.cpp
└── python/
    ├── app.py
    ├── requirements.txt
    └── templates/
        └── index.html
```

---

## 停止运行

- **取消单条生成** — UI 点「停止」
- **关闭守护进程** — `Ctrl+C` 或 `pkill mxy-harnessd`
- **socket 残留** — `rm $XDG_RUNTIME_DIR/mxy-harness.sock`

---

## 已知限制

- 会话 JSON 每次整体重写，几百轮后会慢
- 无并发写保护
- 暂无 WebSocket / RawSocket Provider
- `custom` 模板里的 `{{` 会被当占位符
- UI 未渲染 token 用量

---

## License

MIT © 2024 mcxiaoyang

详见 [LICENSE](LICENSE)。
