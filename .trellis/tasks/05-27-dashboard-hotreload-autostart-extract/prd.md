# trellis-dashboard 自动热更新 + 自启动 + 抽独立 repo

## Goal

把刚交付的 trellis-dashboard（PR1-PR3，见 [[trellis-task-dashboard]]）抽成独立 repo `~/projects/trellis-dashboard/`，让任意 trellis 项目 `cd && trellis-dashboard serve` 就能用；同时补齐 (1) 开发态代码热更新 (2) 系统级自启动 supervised 运行。

## Decision (ADR-lite)

**Context**：dashboard 当前 100% trellis-coupled，住在 `.trellis/scripts/`，跨项目复用要手动 copy；开发态每次改 .py 都要手动重启服务；SSH 进 dev box 就死，需要长开。

**Decision**：
- **抽独立 repo**，受众优先级 = A（自己跨项目复用）但**结构按 D**（pyproject + `src/` 标准包布局，console_scripts 入口，抑留未来 B/C 演进——内网团队分发或公开 OSS）
- **安装方式** = **A1（pipx editable）**：`pipx install -e ~/projects/trellis-dashboard`，命令 `trellis-dashboard` / `trellis-progress`
- **自启动** = **L3（系统优先 + tmux 降级）**：默认装 systemd user unit（`Restart=always` + `enable-linger`），检测 systemd 不可用时把 launcher 脚本写到 `~/.zshrc`
- **热更新**：watcher 监 `src/trellis_dashboard/*.py` mtime（1s debounce）→ 服务 `os.execv` 自重启 + 前端通过 `/api/build-version` 探测版本变化 `location.reload()`；默认关，`serve --watch` 才开
- **GEM5 迁移**：抽出后 GEM5 的 `.trellis/scripts/dashboard*.py` + `progress.py` 走回收站 SOP；`.trellis/config.yaml.hooks` 改调 `trellis-progress`；过渡期保留旧版直到 pipx 装机端到端验证通过

**Consequences**：
- ✅ 一行 `cd && trellis-dashboard serve` 在任何 trellis 项目里用
- ✅ 开发态体验 = 编辑保存即生效（pipx editable + 服务自重启 + 浏览器自刷）
- ✅ 升级到 B/C（内网分发 / PyPI OSS）只需加 CI + `twine upload`，零代码改动
- ✅ trellis 集成接口（hooks / `.current-task` / `progress.jsonl` 路径）100% 保留，行为不变
- ⚠️ GEM5 短期会有"旧脚本 + 新命令"双存的过渡窗口（PR4 收尾才清除）
- ⚠️ enable-linger 在共享 HPC 机器上可能没权限——L3 设计天然兜底为 tmux

## Requirements

- [ ] 标准 Python 包布局：`~/projects/trellis-dashboard/{pyproject.toml, src/trellis_dashboard/*.py, src/trellis_dashboard/web/index.html, LICENSE(MIT), README.md, install/}`
- [ ] `pipx install -e ~/projects/trellis-dashboard` 装通
- [ ] 控制台脚本：`trellis-dashboard` (serve / md / install-service)、`trellis-progress` (emit / done / reset / tail)
- [ ] `cd /任意 trellis 项目 && trellis-dashboard serve` 工作（沿用现有 `_find_repo_root()` 上溯 `.trellis/` 的发现机制）
- [ ] `serve --watch` 文件改动 ≤2s 后服务自重启；前端 `/api/build-version` 变化 ≤3s 自刷
- [ ] `trellis-dashboard install-service` 自动检测：systemd-available → enable-linger → 装 user unit → start；不可用则给 ~/.zshrc 追加 tmux launcher snippet
- [ ] GEM5 `.trellis/config.yaml.hooks` 改调 `trellis-progress`；`.trellis/scripts/{dashboard*.py, progress.py}` 移到 `.recycle_bin/`
- [ ] trellis 现有集成（lifecycle hooks 写 `progress.jsonl`、`.current-task` 自发现）不变

## Acceptance Criteria

- [ ] `pipx install -e ~/projects/trellis-dashboard` 成功，`which trellis-dashboard` 在 PATH
- [ ] 在 GEM5 外的另一个干净 trellis 项目里 `cd && trellis-dashboard serve` 浏览器看到面板
- [ ] `trellis-dashboard serve --watch`，编辑 `src/trellis_dashboard/serve.py`，保存后 ≤2s 服务重启 + 浏览器自动 reload
- [ ] `trellis-dashboard install-service`，`systemctl --user status trellis-dashboard` 显示 active；`systemctl --user restart` 起得来；重启 dev box 后浏览器仍能开
- [ ] systemd 不可用时 `install-service` 写 ~/.zshrc 追加块 + 打印降级说明
- [ ] GEM5 中旧 dashboard*.py 在 `.recycle_bin/`；`task.py start <X>` 触发的 lifecycle hook 仍正确写入 `progress.jsonl`
- [ ] 跨项目验证：在 GEM5 跑 dashboard + 在另一个 trellis 项目用 `--port 47822` 也跑 dashboard，两者数据相互隔离

## Definition of Done

- pipx editable install 在干净机器上可重现
- 热更新机制默认关，不影响生产
- 自启动有明确 enable/disable/状态查询命令（封装在 `trellis-dashboard install-service / uninstall-service / status`）
- 旧 GEM5 dashboard 代码走完回收站 SOP，hooks 改完无残留引用
- README 覆盖：安装、首次跑、热更新、自启动、卸载、跨项目使用
- 抽 repo 的 ADR 写进新 repo 自己的 `docs/`，**不要**留在 GEM5 里

## Out of Scope (临时)

- 多用户认证 / TLS / 反代
- 跨 OS（Windows / macOS）适配——先聚焦 Linux/zsh
- 通用任务追踪器（脱离 trellis schema）
- 多项目单服务（一个 dashboard 进程同时看多个项目）—— 后期 B/C 需求时再考虑
- 公开 PyPI 发布（D 的"现在不发布"）
- CI / 自动化测试管道（PR1 内手测足够；CI 是 B/C 阶段的事）

## Technical Notes

### 当前架构（迁移源）
- 4 个 Python 脚本在 GEM5 `.trellis/scripts/`：`dashboard_data.py`（516）/ `dashboard_serve.py`（1642 含 embedded HTML/CSS/JS）/ `dashboard.py`（248）/ `progress.py`（214）
- 纯 stdlib，零依赖（除浏览器侧 marked.js + mermaid.js via CDN）
- 已落地的 sidecar pattern：见 `.trellis/spec/guides/sidecar-tooling-guide.md`

### 环境约束（已 audit）
- Linux Ubuntu kernel 6.8 / Python 3.12 / zsh / tmux + screen 可用
- **systemd 255 用户级可用**（`systemctl --user --version` 通过）
- `http_proxy=http://172.38.11.182:20170` 设了——内网代理；pipx 装机时可能要 `pipx install -e .` 不依赖网络（editable 不需要下载，OK）

### 目标 repo 布局
```
~/projects/trellis-dashboard/
├── pyproject.toml          # console_scripts + Python>=3.10 + MIT
├── README.md               # 安装 / 使用 / 自启动 / 卸载
├── LICENSE                 # MIT
├── .gitignore              # __pycache__, .venv, dist/
├── docs/
│   └── architecture.md     # 沉淀 ADR：why pipx editable / why subprocess restart
├── src/trellis_dashboard/
│   ├── __init__.py
│   ├── data.py             # 来自 dashboard_data.py
│   ├── snapshot.py         # 来自 dashboard.py 的 md 子命令
│   ├── serve.py            # 来自 dashboard_serve.py 的 Python 部分
│   ├── progress.py         # 来自 progress.py
│   ├── cli.py              # trellis-dashboard 命令分发（serve / md / install-service / status）
│   ├── progress_cli.py     # trellis-progress 命令分发（emit / done / reset / tail）
│   ├── reload.py           # watcher + os.execv 重启（PR2）
│   └── web/
│       └── index.html      # 从 _INDEX_HTML 字符串抽出来的真文件
├── install/
│   ├── systemd/trellis-dashboard.service.in   # 模板，install-service 实例化
│   └── zsh/launcher.sh.in                     # 模板，写入 ~/.zshrc
└── tests/                  # PR1 内最小烟测；正式测试是 B/C 的事
    └── test_smoke.py
```

### Implementation Plan（4 个 PR）

**PR1 — repo 抽出 + pipx editable + 在另一个 trellis 项目里跑通**（foundation）
- 新建 repo 目录 + pyproject + LICENSE + .gitignore
- 把 4 个脚本拷贝过去，按上面布局重命名/拆分；保持外部 CLI 行为完全一致
- 把 `_INDEX_HTML` 字符串抽到 `web/index.html` 由 serve.py 在启动时 read（用 `importlib.resources` 让 pipx 装完后能找到）
- `pipx install -e ~/projects/trellis-dashboard` 装通
- 在 GEM5 外的一个临时 trellis 测试项目验证

**PR2 — 热更新**（dx）
- `reload.py` 监 `src/trellis_dashboard/**/*.py` + `web/index.html` 的 mtime
- 1s debounce（避免 vim 4913 假触发）
- 检测到变化 → `os.execv(sys.executable, [sys.executable] + sys.argv)` 自重启
- 服务启动时计算源文件哈希（sha256 全包 .py + html），暴露 `/api/build-version`
- 前端在 poll() 里同时检查 build version，变了就 `location.reload()`
- `serve --watch` 才启用；默认关

**PR3 — 自启动**（ops）
- `install/systemd/trellis-dashboard.service.in` 模板，含 `${TRELLIS_PROJECT}` / `${PIPX_BIN}` 占位
- `install/zsh/launcher.sh.in` 模板，含 tmux session 启动 + idempotent 检测
- 新命令：
  - `trellis-dashboard install-service [--project /path] [--port N]` —— 检测 systemd → enable-linger → 实例化模板写入 `~/.config/systemd/user/` → `daemon-reload` + `enable --now`；失败降级追加 ~/.zshrc snippet
  - `trellis-dashboard uninstall-service`
  - `trellis-dashboard status` —— 显示 systemd 状态 + 进程 + 监听端口
- 默认 `--project` = 当前 cwd 的 trellis 根；`--port` = 47821

**PR4 — GEM5 善后迁移**（cleanup）
- GEM5 `.trellis/scripts/{dashboard*.py, progress.py}` → `.recycle_bin/`（用 git mv，按回收站 SOP）
- `.trellis/config.yaml.hooks` 把 `python3 ./.trellis/scripts/progress.py emit ...` 改成 `trellis-progress emit ...`
- 更新 GEM5 仓库根 `DASHBOARD.md`：从"本地脚本"改成"通过 trellis-dashboard 命令使用"+ 安装指引
- 验证：`task.py start/finish` 仍正确触发 progress 事件写入
- 最后归档本任务和 [[trellis-task-dashboard]] 父任务

### 关键技术选择
- **包源**：`src/` layout 而不是 flat layout——更标准，避免 import 路径污染，PyPI 发布友好
- **HTML 加载**：`importlib.resources.files("trellis_dashboard.web").joinpath("index.html").read_text()`——pipx 装完仍能找到
- **热更新**：选 `os.execv` 自重启而不是 watchdog/uvicorn-style 子进程隔离——KISS，stdlib only
- **systemd 模板**：`.in` 后缀 + 简单 envsubst-style 替换，不引入 jinja2

### 跨会话衔接
- 这个任务由 trellis-brainstorm 创建，task dir 在 GEM5 仓库内
- **新会话开始时**：`python3 .trellis/scripts/task.py start 05-27-dashboard-hotreload-autostart-extract` 已激活，SessionStart hook 会注入这个任务
- 子任务 PR1-PR4 已创建为 children；按 ID 顺序推
- 推荐第一步：进入 PR1 后跑 `mkdir -p ~/projects/trellis-dashboard && cd ~/projects/trellis-dashboard && git init`
