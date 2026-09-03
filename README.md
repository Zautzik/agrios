# Agrios

[![CI](https://github.com/Zautzik/agrios/actions/workflows/ci.yml/badge.svg)](https://github.com/Zautzik/agrios/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Python 3.12](https://img.shields.io/badge/python-3.12-blue.svg)](pyproject.toml)

**A LangGraph ReAct-style agent for farm operations, running entirely on a local Ollama model — SQL-backed persistence, full tracing, and explicit failure handling at every external boundary. Scored 28/30 on a 30-case live-model eval suite** ([details](#evaluation)).

```
                ┌──────────┐
   START ─────► │  agent   │ ◄────────┐
                └────┬─────┘          │
                     │  tool_calls?   │
              ┌──────┴──────┐         │
             yes            no        │
              ▼              ▼        │
         ┌─────────┐       END        │
         │  tools  │ ────────────────-┘
         └─────────┘
```

Full build log → [NOTES.md](NOTES.md) &nbsp;·&nbsp; From-first-principles study guide → [docs/Masterclass-Agrios.md](docs/Masterclass-Agrios.md)

This is a learning project, built incrementally and documented as it went — including a real debugging story and the engineering tradeoffs made along the way.

## What it does

A CLI agent that answers questions about weather, crop calendars, and field/task status for a small set of farms, calling out to tools when it needs real data instead of guessing. Conversations persist across restarts (same `thread_id` resumes where it left off), and every run is traceable end-to-end — full input/output and tool-call spans, not just the final printed answer.

```
You: What's the weather forecast for field-1, and should I log an irrigation task?
[agent reasons -> calls get_weather_forecast -> reasons -> calls log_task -> responds]
```

## Demo

A real, unedited transcript from a live run against `qwen2.5:7b` — a plain conversational turn followed by a tool-calling turn resolved against the actual SQLite-backed field data:

```
You: Hi, who are you?
Hello! I'm Qwen, an AI assistant created by Alibaba Cloud. My main goal is to help you with
any questions or tasks related to agriculture, weather forecasts, and more. How can I assist
you today? Whether you need information about planting, managing fields, or anything else
related to your farming activities, feel free to ask!

You: What is the status of field-1?
The current status of field-1 is that it contains potatoes which are currently growing.

Would you like to log a task for field-1 or need more information about the crop?
```

(`query_field_status(field_id="field-1")` was called under the hood and returned the real row from `agrios.db`.)

## Architecture

The graph shown at the top of this README, in full:

- **`agent` node** — calls the Ollama model (bound to 5 tools) with the running message history.
- **`tools` node** — executes whichever tool(s) the model requested, returns results as `ToolMessage`s.
- **Conditional edge** — routes back to `tools` only while the model keeps requesting them; exits to `END` the moment it answers directly.
- **Checkpointing** — `SqliteSaver` persists graph state per `thread_id` to `checkpoints.db`, so multi-turn memory survives a process restart, not just a single run.

## Design decisions

A few choices here were deliberate, not defaults — worth knowing why if you're reading the code:

- **Recursion limit set explicitly (`10`), not left at LangGraph's default (`25`).** The `agent → tools → agent` cycle's exit condition depends entirely on model judgment; a local 7B model can get stuck re-calling a tool with bad args. Capping it tighter than the library default catches a stuck loop faster, and it's caught explicitly via `GraphRecursionError` so the CLI degrades to a message instead of crashing.
- **Exception handling is scoped to where failure is actually possible, not blanket-applied.** The three tools that touch SQLite catch `SQLAlchemyError`, scoped to real I/O. The two tools that return static data don't have a try/except at all — there's nothing there that can fail yet, and a bare `except Exception` around code that can't throw just masks future bugs instead of guarding anything real. At the model-call boundary, the ollama client's streaming and non-streaming code paths have different exception contracts: `_request_raw()` wraps `httpx.ConnectError` in Python's builtin `ConnectionError`, but the streaming path used by `ChatOllama` (`inner()`) does not — it lets `httpx.ConnectError` propagate unwrapped. Catching the right types required reading the client source, not guessing from the class hierarchy. The except tuple covers `ollama.ResponseError` (model-side error), `ConnectionError` (non-streaming path, server unreachable), and `httpx.ConnectError` (streaming path, TCP connection refused) — each for a confirmed failure mode, not defensive coverage.
- **Domain data sits behind a SQLAlchemy engine, not raw `sqlite3` calls.** Every tool goes through one `engine` object with explicit connection pooling (`QueuePool`). At this scale that's not solving a real concurrency problem — it's a single-threaded CLI — but it means the only thing that changes to move to Postgres later is the connection string on one line; no tool function needs to change.
- **Database paths are anchored to the project directory, not CWD.** Both `agrios.db` and `checkpoints.db` resolve through `Path(__file__).parent` rather than relative URI strings. A relative path in a SQLAlchemy connection string (`sqlite:///file.db`) resolves to whatever the process's current working directory is at runtime — not where the code lives. That's an ambient property of how the script was invoked, not a stable code decision. Anchoring to `__file__` makes the file location an explicit invariant rather than something that silently varies depending on where you launched the process from.
- **Tracing via self-hosted Langfuse, not LangSmith.** A `CallbackHandler` is wired into the LangGraph `invoke` config, giving full per-node trace spans (model input/output, tool name/args/result) without depending on a paid hosted service.

## Tools

| Tool | Purpose |
|---|---|
| `get_weather_forecast(location, days=7)` | Forecast for planting/irrigation/frost-risk timing |
| `lookup_crop_calendar(crop, region)` | Planting and harvest windows for a crop |
| `query_field_status(field_id)` | Current crop and status for a field |
| `log_task(field_id, description)` | Records a pending task against a field |
| `get_pending_tasks()` | Lists all pending tasks across fields |

`get_weather_forecast` and `lookup_crop_calendar` currently return fixture data — they're shaped to be swapped for real API calls without changing their interface.

## Perception/motor-control bridge (`cpp/`)

A standalone lock-free single-producer/single-consumer ring buffer over POSIX shared memory,
carrying 6-DoF pose data between a Python perception process and a C++ motor-control process —
no lock, no kernel round-trip on the hot path. Not yet wired into the LangGraph agent above; it's
an independent subsystem in this repo, built for a future robotics-hardware target (Raspberry Pi
/ Jetson) rather than the current CLI/laptop setup. Verified end to end in a Linux container: a
5,000,000-item concurrent stress test with zero lost/duplicated/reordered items, and a real
cross-process run (separate C++ and Python processes, genuine shared memory, not simulated) with
20/20 poses arriving correctly. See [cpp/README.md](cpp/README.md) for the design, the memory-
ordering contract, and how to build and run it — requires Linux/macOS (or a container), since
POSIX shared memory doesn't exist on native Windows.

## Evaluation

A 30-case parametrized suite (`test_agent.py`, run via `pytest -m slow`) checks the live model
across four categories: tool selection (15 cases), argument correctness (8), multi-step task
completion (5), and no-tool-needed (2). It's gated behind a marker so a plain `pytest` run stays
fast and never touches Ollama; the full suite takes ~25 minutes on CPU against `qwen2.5:7b`.

**Reconciled result** (original run + one test-bug fix, not a single re-run): **28/30 cases
correct.**
- 1 confirmed, reproducible gap: the literal phrasing *"What's the weather forecast for
  field-1?"* — wording closest to the tool's own docstring — doesn't trigger
  `get_weather_forecast` (3/3 across independent isolated runs), while paraphrases of the same
  request reliably do. Not yet fixed.
- 1 open question: a single recursion-limit hit on a frost-risk question didn't reproduce on
  retry (1 fail, 1 pass across everything actually run) — too little data to call it resolved or
  call it a bug, so it's a watch item.

See [NOTES.md](NOTES.md) for the full investigation, including a hypothesis (cold-start effect)
that got tested and killed before either finding was written down.

## Stack

- [LangGraph](https://github.com/langchain-ai/langgraph) — agent control flow as an explicit graph
- [LangChain](https://github.com/langchain-ai/langchain) + [`langchain-ollama`](https://github.com/langchain-ai/langchain) — model/tool binding
- [Ollama](https://ollama.com/) (`qwen2.5:7b`) — local inference, no API cost or external dependency
- [SQLAlchemy](https://www.sqlalchemy.org/) — pooled connections over SQLite (`agrios.db`), portable to Postgres
- `langgraph-checkpoint-sqlite` — conversation persistence (`checkpoints.db`)
- [Langfuse](https://langfuse.com/) (self-hosted) — full tracing/observability
- [`uv`](https://github.com/astral-sh/uv) — dependency management

## Setup

**Prerequisites:** [Ollama](https://ollama.com/download) installed and running, [`uv`](https://github.com/astral-sh/uv) installed, [Docker Desktop](https://www.docker.com/products/docker-desktop/) (for tracing).

```bash
# 1. Pull the model
ollama pull qwen2.5:7b

# 2. Install dependencies
uv sync

# 3. (optional) bring up self-hosted Langfuse for tracing
git clone https://github.com/langfuse/langfuse.git langfuse-server
cd langfuse-server && docker compose up -d
# open http://localhost:3000, sign up, create a project, generate an API key
```

Create a `.env` in the project root:
```
LANGFUSE_PUBLIC_KEY="pk-lf-..."
LANGFUSE_SECRET_KEY="sk-lf-..."
LANGFUSE_BASE_URL="http://localhost:3000"
```

Run it:
```bash
uv run main.py
```
Type a message at the `You:` prompt; type `exit` to quit. Tables and seed data (`agrios.db`) are created automatically on first run.

## Known limitations

- `get_weather_forecast` and `lookup_crop_calendar` return fixture data, not live API results.
- Single `thread_id` ("1") is hardcoded — no per-user session handling. In practice this also means history only grows: after ~2 months of development sessions the persisted thread had accumulated 25+ messages, and every subsequent turn replays all of it through the model before generating — confirmed to noticeably slow interactive turns (30s–2min on CPU) that would otherwise be quick. No trimming/summarization is implemented yet.
- `qwen2.5:7b` needs ~4.3 GB free RAM to load; on a constrained machine running Docker simultaneously, this can fail — handled gracefully (see Design decisions) rather than crashing, but worth knowing going in.
- Langfuse trace export is currently failing in local verification (`Read timed out`, then `404 Not Found` from the OTLP endpoint) — traces are not being recorded even though the agent itself runs fine, since export happens on a non-blocking background path. Root cause not yet investigated; most likely an SDK/server version mismatch on the self-hosted instance.
