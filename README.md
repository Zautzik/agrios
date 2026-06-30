# Agrios

A LangGraph ReAct-style agent for farm operations, running entirely on a local Ollama model, with SQL-backed persistence, full tracing, and explicit failure handling at every external boundary.

This is a learning project, built incrementally and documented as it went — see [NOTES.md](NOTES.md) for the build log, including a debugging story and the engineering tradeoffs made along the way.

## What it does

A CLI agent that answers questions about weather, crop calendars, and field/task status for a small set of farms, calling out to tools when it needs real data instead of guessing. Conversations persist across restarts (same `thread_id` resumes where it left off), and every run is traceable end-to-end — full input/output and tool-call spans, not just the final printed answer.

```
You: What's the weather forecast for field-1, and should I log an irrigation task?
[agent reasons -> calls get_weather_forecast -> reasons -> calls log_task -> responds]
```

## Architecture

```
                ┌──────────┐
   START ─────► │  agent   │ ◄────────┐
                └────┬─────┘          │
                     │                │
         tool_calls? │                │
            ┌────────┴────────┐       │
            │                 │       │
           yes                no      │
            │                 │       │
            ▼                 ▼       │
       ┌─────────┐          END       │
       │  tools  │ ─────────────────────┘
       └─────────┘
```

- **`agent` node** — calls the Ollama model (bound to 5 tools) with the running message history.
- **`tools` node** — executes whichever tool(s) the model requested, returns results as `ToolMessage`s.
- **Conditional edge** — routes back to `tools` only while the model keeps requesting them; exits to `END` the moment it answers directly.
- **Checkpointing** — `SqliteSaver` persists graph state per `thread_id` to `checkpoints.db`, so multi-turn memory survives a process restart, not just a single run.

## Design decisions

A few choices here were deliberate, not defaults — worth knowing why if you're reading the code:

- **Recursion limit set explicitly (`10`), not left at LangGraph's default (`25`).** The `agent → tools → agent` cycle's exit condition depends entirely on model judgment; a local 7B model can get stuck re-calling a tool with bad args. Capping it tighter than the library default catches a stuck loop faster, and it's caught explicitly via `GraphRecursionError` so the CLI degrades to a message instead of crashing.
- **Exception handling is scoped to where failure is actually possible, not blanket-applied.** The three tools that touch SQLite catch `SQLAlchemyError`, scoped to real I/O. The two tools that return static data don't have a try/except at all — there's nothing there that can fail yet, and a bare `except Exception` around code that can't throw just masks future bugs instead of guarding anything real. Same logic at the model-call boundary: `ollama.ResponseError` (model responded with an error, e.g. out-of-memory) and `ConnectionError` (Ollama unreachable) are caught specifically, by reading what the `ollama` client actually raises, rather than guessed at.
- **Domain data sits behind a SQLAlchemy engine, not raw `sqlite3` calls.** Every tool goes through one `engine` object with explicit connection pooling (`QueuePool`). At this scale that's not solving a real concurrency problem — it's a single-threaded CLI — but it means the only thing that changes to move to Postgres later is the connection string on one line; no tool function needs to change.
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
- Single `thread_id` ("1") is hardcoded — no per-user session handling.
- `qwen2.5:7b` needs ~4.3 GB free RAM to load; on a constrained machine running Docker simultaneously, this can fail — handled gracefully (see Design decisions) rather than crashing, but worth knowing going in.
