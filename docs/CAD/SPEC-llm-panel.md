# In-app LLM assistant: Preferences + control panel

Status: spec from interview 2026-11-09. Nothing implemented. Lands on `epic/solidcraft`, then cherry-picked to `epic/solidcraft-upstream`.

## Baseline (measured by reading, not by running)
- FreeCAD: no LLM setting in the app. `frecad-console` -> XML-RPC :9875; agent mode -> `freecad-claude-bridge` :7891 -> `claude --mcp-config` (model + login owned by the CLI).
- SolidCraft: reached from outside via an HTTP MCP server (claude.ai connector). No in-app LLM setting found (only `solidcraft_work` searched).
- Orca-CAD: MCP over unix socket (`McpControl.cpp`); no in-app LLM.

## Decisions (revised 2026-11-09: single user, everything on one PC)
Deployment: one user, Orca-CAD and the agent CLIs (claude-cli, opencode, antigravity/agy, codex-cli) run on the SAME machine. No server instance, no remote clients.

| Topic | Decision |
|---|---|
| Where | Main Orca Preferences, new chapter "LLM / MCP". |
| Backends | (a) Local agent CLI, spawned by the app: claude, opencode, agy, codex. The CLI owns its login and model unless overridden. (b) Direct HTTP API: Anthropic, OpenAI-compatible (base URL + key), Ollama. |
| Per-backend fields | CLI: executable (auto-detected on PATH), extra args, model override. API: model, base URL, token. |
| MCP | App listens on loopback only (unix socket, plus loopback TCP port for CLIs that need HTTP). Port claimed from the majordomo pool, shown in Preferences. App writes the MCP config each CLI needs (e.g. `--mcp-config`) when it spawns it. |
| Network | No all-interfaces toggle, no bearer token. A non-loopback bind is refused outright. |
| Token | Only for API backends: OS keyring, fallback env-var name. Never in config file, 3MF or logs. |
| Tools | Full existing MCP verb set. CLIs reach it through MCP; API backends call it in-process. |
| Panel | One widget: dockable in the Design tab + pop-out window; remembers mode. Shows the agent's text and each tool call live. |
| Input | Text; images (paste/drag-drop; saved to a temp file path for CLIs, vision input for APIs); viewport-screenshot button; STEP/CAD via Import CAD; text/PDF/CSV inlined or passed as path. |
| Safety | A turn's edits form ONE undo group. No per-call approval. |
| History | Per-document chat stored in the design file, with a "do not store" switch. |

## Risks to resolve in phase 1/2
1. Chat in the 3MF travels with shared files (prompts, images) and the repo is public: add a "do not store chat" switch, strip on export, downscale images.
2. A loopback TCP port is still reachable by any local process: acceptable for a single-user PC, but say so in the Preferences tooltip.
3. Keyring availability on headless/Linux rig: needs the env-var fallback to be testable.
4. In-process tool calls must run on the UI thread or via the existing McpControl dispatch; no new threading model.
5. `boost::process`/HTTP client in libslic3r on macOS/Windows: reuse what Orca already links (libcurl via `Http`), do not add a dependency.

## Phases (each built, shown on his screen per Rule 13, then pushed)
1. Preferences chapter + loopback MCP listener + CLI launcher (claude first). Check: panel-less run, app spawns `claude -p` against the MCP and it calls `describe_tools`; non-loopback bind refused.
2. Panel (dock) driving the claude CLI backend (text chat, live tool calls); one undo group per turn; per-document history. Check: "add a 20 mm cube" builds it and Ctrl+Z removes it in one step.
3. Attachments, remaining CLIs (opencode, agy, codex) and API backends (Anthropic, OpenAI-compatible, Ollama) with keyring token, pop-out window.

## Not decided
- Exact Preferences tab placement (Behaviour tab holds the CAD checkboxes today).
- System prompt content and per-turn token budget.
- Streaming vs whole-reply rendering in phase 2 (default: streaming if `Http` supports it).
