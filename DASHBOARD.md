# Trellis Task Dashboard

Lightweight, **read-only** view of every active trellis task — status, phase
progress, and the AI's current sub-step.

> **As of PR4 (2026-05-27)**: the dashboard now lives as a standalone Python
> package in `~/projects/trellis-dashboard/`, installed via `pipx`. The old
> `.trellis/scripts/dashboard*.py` + `progress.py` have been retired to
> `.recycle_bin/.trellis/scripts/`. This file documents how to use the new
> `trellis-dashboard` / `trellis-progress` commands from inside the GEM5 repo.
> Trellis integration (lifecycle hooks → `progress.jsonl`, `.current-task`
> auto-discovery, sidecar layout) is unchanged.

## 0. Install (one-time, per dev box)

```bash
pipx install -e ~/projects/trellis-dashboard
which trellis-dashboard trellis-progress     # → ~/.local/bin/...
```

To autostart the live server on login (systemd user unit, falls back to
`~/.zshrc` tmux launcher):

```bash
trellis-dashboard install-service --port 47821
systemctl --user status trellis-dashboard    # verify
```

See `~/projects/trellis-dashboard/README.md` for full install / uninstall /
hot-reload details.

---

## 1. What you get today (PR1 + PR2 + PR3, now packaged)

| Component | Purpose |
|---|---|
| `trellis_dashboard.data` | Pure data layer: scans `.trellis/tasks/**`, parses `progress.jsonl`, returns a JSON-serializable state dict |
| `trellis-progress` (CLI) | Append progress events to `.trellis/tasks/<dir>/progress.jsonl` |
| `trellis-dashboard md` | Markdown snapshot generator (consumes the data layer) |
| `trellis-dashboard serve` | **Read-only HTTP live view** (stdlib server, polls every 2.5 s) |
| `.trellis/config.yaml` `hooks:` | Auto-emit coarse `lifecycle` events on `task.py start` / `finish` (now calls `trellis-progress`) |
| `.trellis/.gitignore` | Ignores `progress.jsonl` + workspace dashboard artifacts |

---

## 2. Quickstart

### See the snapshot

```bash
trellis-dashboard md
# → wrote .trellis/workspace/<you>/DASHBOARD.md

trellis-dashboard md --stdout       # to stdout
trellis-dashboard md --archive      # include archived
```

### Emit fine-grained progress (recommended during long tasks)

```bash
# Pick the current task automatically
trellis-progress emit --current \
    --phase 2.1 --step "调研 fetch.cc 现有 hook 点" --pct 40

# Or specify by slug / task dir
trellis-progress emit 04-23-btb-predecode-candidate-consumer \
    --phase 2.1 --step "补 unit test 3/5" --pct 65

# Mark done explicitly
trellis-progress done --current

# Inspect / reset
trellis-progress tail --current -n 10
trellis-progress reset --current
```

### Inspect raw state JSON (for debugging / scripts)

```bash
# Use the pipx-managed venv's python so the module resolves
~/.local/share/pipx/venvs/trellis-dashboard/bin/python -m trellis_dashboard.data | jq '.kpis'
~/.local/share/pipx/venvs/trellis-dashboard/bin/python -m trellis_dashboard.data --archive | jq '.tasks | length'
```

---

## 3. Live server

Default port **47821** (high, unassigned, low collision risk), bound to
`127.0.0.1` (loopback only — safe by default).

```bash
# On the dev host
trellis-dashboard serve
# Server prints the exact SSH tunnel command, then keeps running until Ctrl-C.

# Other useful flags
trellis-dashboard serve --port 9999
trellis-dashboard serve --archive          # include archived tasks
trellis-dashboard serve --host 0.0.0.0     # LAN access (only if you really need it)
trellis-dashboard serve --watch            # PR2 dev mode: auto-restart on src changes
```

```bash
# On your laptop
ssh -L 47821:localhost:47821 <dev-host>
# then open http://localhost:47821 in any browser
```

The page polls `/api/state` every 2.5 s; you'll see new `progress emit` events
appear without refreshing. Top-right indicator shows live / reconnecting state.

### Endpoints
| Route | Purpose |
|---|---|
| `GET /` | Single-page HTML view (KPI + filter bar + Kanban + Tree + Mermaid graph + activity timeline + detail modal) |
| `GET /api/state` | JSON dump from `dashboard_data.build_state()`. Accepts `?archive=1` to include archived rows per request. |
| `GET /api/task/<id-or-dir>` | Per-task detail bundle: full task.json, raw prd.md/info.md, all progress events, jsonl context, file listing |
| `GET /api/build-version` | (PR2) sha256 of packaged sources; lets the page detect a hot-reload and `location.reload()` |
| `GET /healthz` | Returns `ok\n` for tunnel/liveness probes |

### Filter & search bar (top of page)
- **Free-text search** — matches against title / dir / branch / id (debounced)
- **Status / Priority / Assignee** dropdowns — auto-populated from current data
- **`stagnated only`** — show only `in_progress` tasks with no progress event for &gt; N days
- **`include archive`** — re-fetches from `/api/state?archive=1` and merges archived tasks into all views
- **`stale > N d`** — threshold for stagnation detection (default 3 days)
- Filter state is reflected in the URL as query params (`?status=in_progress&assignee=glr&stagnant=1`), so you can bookmark / share specific views
- `reset` button clears all filters

### Stagnation alerts
A task is flagged stagnant when:
- `status == "in_progress"` (planning sitting idle is fine; we don't nag about that)
- AND `now - last_activity_ts > staleDays` (default 3 days)
- Last activity = max of (task.json mtime, prd.md mtime, last progress event ts)

Visual treatment: orange border on Kanban card / Tree node + `⚠ stagnant Nd` chip
on the meta line + orange stroke on the Mermaid node + KPI `Stagnant: N` counter.

### Dependency graph (mermaid)
A `graph TD` view rendered with mermaid.js (loaded from jsDelivr CDN, silent
fallback to "not available" text if blocked).
- Nodes coloured by status (planning / in_progress / blocked / done)
- Current task and stagnant tasks get distinctive strokes
- **Solid arrows** = parent → child
- **Dashed arrows** = `relatedFiles` cross-references (when the referenced task is also visible under the current filter)
- **Click any node** → opens the same detail modal as the Kanban / Tree
- Filter bar narrows the graph too (only filtered nodes appear)

### Detail modal
Click any Kanban card or Tree node title → a side modal slides open with:
- All task.json metadata (priority, branch, parent, children, related, PR link, ...)
- **Rendered prd.md** (markdown → HTML via marked.js loaded from jsDelivr CDN;
  silent fallback to raw `<pre>` if blocked / offline)
- info.md (if present)
- Full progress event history (not just the latest, newest-first)
- File listing for the task directory
- Collapsible raw task.json

URL hash routing: opening a task sets `#task=<id>`, so you can deep-link or
bookmark a specific task. Browser back/forward and Esc all dismiss the modal.
Parent/children/related references inside the modal are clickable pills that
navigate to that task's detail.

### Behind an HTTP proxy?

If your shell has `http_proxy` / `HTTPS_PROXY` set, your browser may try to
route `localhost:47821` through it too. Add `localhost,127.0.0.1` to your
browser / system `no_proxy` list. The server itself doesn't care; this only
affects clients that misuse the proxy for loopback addresses.

---

## 4. Progress event protocol

`trellis-progress emit` appends a single JSON object per line to
`.trellis/tasks/<task-dir>/progress.jsonl`:

```json
{"ts": "2026-05-25T18:10:04Z", "phase": "2.1", "step": "implementing X", "pct": 65, "status": "in_progress", "source": "manual"}
```

| Field | Notes |
|---|---|
| `ts` | UTC ISO8601, set automatically |
| `phase` | Free-form. Recommended: workflow phase id (`1.3`, `2.1`, ...) or action name (`implement`, `check`) |
| `step` | **Required.** Short human-readable current step |
| `pct` | Optional 0..100. Used by the dashboard progress bar |
| `status` | `in_progress` \| `blocked` \| `done` \| `error` (optional) |
| `source` | `manual` \| `hook` \| `skill` \| `ci` (free-form, drives icons later) |
| `note` | Optional longer text |

### When should the AI emit?

This is a **project convention** (sidecar — *not* baked into upstream trellis
skill prompts, to avoid `.template-hashes.json` conflicts):

1. **Always** at the start of a non-trivial phase: `trellis-progress emit --current --phase <X.Y> --step "<what I'm starting>" --pct <N>`
2. At each meaningful milestone within a phase (e.g. "wrote module A", "tests pass", "stuck on Y")
3. Before a long-running command (so the dashboard shows what's blocking)
4. On `done`: either let `after_finish` hook fire, or call `trellis-progress done --current`

The lifecycle hooks already cover the coarse `task started` / `task finished`
events; AI-emitted events fill the sub-step gap.

---

## 5. Files & upgrade safety

| Path | Tracked by trellis? | Touched by this feature? |
|---|---|---|
| `~/projects/trellis-dashboard/src/trellis_dashboard/data.py` | n/a (separate repo) | created (PR1) |
| `~/projects/trellis-dashboard/src/trellis_dashboard/snapshot.py` | n/a | created (PR1) |
| `~/projects/trellis-dashboard/src/trellis_dashboard/serve.py` | n/a | created (PR1 + PR2) |
| `~/projects/trellis-dashboard/src/trellis_dashboard/progress.py` | n/a | created (PR1) |
| `.trellis/scripts/dashboard.py` | No (was untracked) | **moved to `.recycle_bin/.trellis/scripts/` (PR4)** |
| `.trellis/scripts/dashboard_data.py` | No | **moved to `.recycle_bin/.trellis/scripts/` (PR4)** |
| `.trellis/scripts/dashboard_serve.py` | No | **moved to `.recycle_bin/.trellis/scripts/` (PR4)** |
| `.trellis/scripts/progress.py` | No | **moved to `.recycle_bin/.trellis/scripts/` (PR4)** |
| `.trellis/config.yaml` | Yes — official extension point | hooks block now calls `trellis-progress` (PR4) |
| `.trellis/.gitignore` | No | appended dashboard ignores |
| `.trellis/workflow.md` | Yes | **not modified** |
| `.trellis/scripts/task.py` / `common/*` | Yes | **not modified** |
| `.claude/skills/*` / `.codex/agents/*` / `.claude/hooks/*` | Yes | **not modified** |
| `DASHBOARD.md` (this file) | No | rewritten for new commands (PR4) |

`trellis upgrade` will at most prompt to 3-way-merge `config.yaml` (the only
tracked file we touched, and that's what its `hooks:` block is designed for).
