# Agrios — Masterclass & Study Guide
> A from-first-principles study guide to everything living in this repo: a ~170-line LangGraph
> agent that turned out to contain a full syllabus — explicit state machines, functional state
> reducers, durable checkpointing, tool-calling as structured generation, bounded recursion,
> failure-boundary discipline, connection pooling, observability, and hermetic testing of a
> non-deterministic system. Small codebase, complete concepts. Every idea below is traceable
> to a specific line in `main.py`, `test_agent.py`, or a specific commit — nothing here is
> invented, and nowhere does this document claim a measured result the project doesn't
> actually have. The eval suite has since been run for real: 28/30 on reconciliation, one
> confirmed reproducible gap, one open question — see war story #7 in Part 3.
>
> Companion to [NOTES.md](../NOTES.md) (the build log, in the moment) and [README.md](../README.md)
> (the reference). This document is the third angle: not what happened or what it does, but
> *why the ideas underneath it work*, explained as if teaching them to someone who's never
> seen an agent framework before.
---
## Part 0 — The one-sentence pitch
> *"Agrios is a LangGraph ReAct-style agent for farm operations, running entirely on a local
> Ollama model: five tools, a hand-written state graph instead of a black-box `.run()` call,
> SQL-backed persistence that survives a restart, full tracing, and exception handling scoped
> to exactly the failures I've confirmed can happen — including two I only found by running the
> thing and reading a library's source code."*
The project is deliberately small. That's not a limitation to apologize for — it's what makes
every concept in it inspectable in full, instead of buried under abstraction layers you have to
take on faith. A production agent framework hides the graph, the reducer, the checkpoint
format, and the tool-calling wire protocol behind convenience functions. This project wrote
all of them out by hand, once, on purpose, specifically so each one could be understood rather
than assumed.
---
## Part 1 — Skills inventory
Depth is marked honestly: **solid** (could defend it at a whiteboard) · **developing** (understand
it, want more reps).
### 1A. Technical / hard skills
| Skill | What I can now do | Depth |
|---|---|---|
| **Agent control flow as an explicit graph** | Model an agent loop as nodes + edges (`StateGraph`) instead of a hidden `.run()` — explain why that buys checkpointing, tracing, and inspectability that a black-box loop can't | solid |
| **State & reducers** | Explain why `Annotated[list, add_messages]` is required for multi-turn memory to work at all, and what silently breaks without it | solid |
| **Conditional routing** | Write a routing function (`should_continue`) that inspects the last message and returns a node name or `END` — the mechanism every agent loop's exit condition reduces to | solid |
| **Durable checkpointing** | Swap `MemorySaver` → `SqliteSaver` as a storage-layer change behind one interface; explain what a checkpoint actually stores and why `thread_id` is the partition key | solid |
| **Tool / function calling** | Explain the path from a Python function + docstring to a JSON schema to a model's structured `tool_calls` output to an executed result fed back as a `ToolMessage` | solid |
| **Bounded recursion** | Set an explicit `recursion_limit`, explain why an agent/tools cycle has no built-in termination guarantee, and handle `GraphRecursionError` as a real, expected outcome | solid |
| **Narrow exception handling** | Scope every `except` to a boundary where a *specific, confirmed* failure can occur; delete a `try/except` that guards nothing rather than widen it | solid |
| **Reading library source over guessing** | Traced a bug through `ollama`'s Python client to two different exception contracts on two different code paths (streaming vs. non-streaming) — found by reading, not by pattern-matching a traceback | solid |
| **SQL via SQLAlchemy** | Move from raw `sqlite3` calls to a pooled `engine` + `text()`, understand what `QueuePool` actually does, and why the real payoff is a one-line connection-string swap, not the pooling itself at this scale | solid |
| **Environment vs. code invariants** | Diagnose a bug caused by a CWD-relative path resolving differently depending on where a process was launched from; fix it by anchoring to `Path(__file__).parent` | solid |
| **Observability** | Wire a `CallbackHandler` into a graph invocation for full per-node tracing (self-hosted Langfuse), distinct from and complementary to stdlib `logging` | solid |
| **Local LLM inference** | Run tool-calling inference entirely offline via Ollama; understand the real RAM ceiling this imposes, having hit it | solid |
| **Testing a non-deterministic system** | Design a parametrized eval suite across failure-mode categories, keep it hermetic against a system whose whole point is persistent shared state, and gate a slow/live suite behind a marker | solid |
| **Python craft in service of the above** | `TypedDict` + `Annotated` for typed, reducer-aware state; decorators (`@tool`); context managers (`with engine.connect()`, `with SqliteSaver.from_conn_string(...)`); dict-merge semantics; f-strings; `if __name__ == "__main__"` | solid |
| **Git workflow** | Five commits, each a coherent unit of work with a rationale in the message — not "fix stuff" | solid |
### 1B. Cognitive habits
These outlast this specific stack. An interviewer can teach a hire `SqliteSaver`; these are harder to teach.
| Habit | The move | Where I did it |
|---|---|---|
| **Read the source, not the stack trace** | When a caught-exception assumption turned out wrong, went into the `ollama` client's actual code instead of widening the `except` | The `httpx.ConnectError` bug — two code paths, two different exception contracts, found at specific line numbers |
| **Isolate & inspect** | When output didn't match the code I was reading, stopped trusting the editor tab | The stale-interpreter `NameError` — the fix was already saved to disk, the running process wasn't the saved file |
| **Make inherited defaults into decisions** | Noticed I was relying on LangGraph's default recursion limit (25) without ever having chosen it | Set it explicitly to 10, with a reason (a 7B model can misread a tool result and re-call it) |
| **Delete code that isn't guarding anything** | Two tools had a `try/except Exception` around a hardcoded string return — no I/O, nothing that could fail | Removed it instead of narrowing it; a wider or narrower `except` around a function that can't throw is still theater |
| **Don't build for a step you haven't taken yet** | The SQLAlchemy engine makes a future Postgres swap a one-line change — and stops there. No Postgres code has been written, because none has been *needed* yet | The open question in NOTES.md names this explicitly rather than pretending it's already validated |
| **Test hermetically, not against shared state** | Recognized that the app's own persistence (`SqliteSaver` + fixed `thread_id`) would make eval cases leak into each other if reused as-is | `MemorySaver` + a fresh `uuid4()` thread per test case |
| **Name the environment as a bug source, not just the code** | An OOM crash and a CWD-relative path bug were both "correct code, wrong assumption about what's running underneath it" | The Docker/WSL memory-pressure incident; the subdirectory-launch path bug |
| **Predict-then-verify** | State a plausible explanation for a failure, then test it directly instead of writing it down as fact | The cold-start theory for the weather-forecast miss — killed by rerunning the case alone twice more instead of trusting the first guess |
---
## Part 2 — Where the project stands
Every phase below is one real commit — this is the actual build order, not a retrofit narrative.
| Phase | Commit | What changed | The concept it introduced |
|---|---|---|---|
| 0 — Scaffold | `eaf0590` | `create_react_agent` + one tool (`get_weather_forecast`), a single `.invoke()`, no memory | The prebuilt agent constructor: convenient, opaque |
| 1 — Memory | `e6c266d` | Swapped to `langchain.agents.create_agent`, added `MemorySaver` + `thread_id`, replaced the one-shot invoke with an `input()` loop | Checkpointing exists, but only for the life of the process |
| 2 — The real graph | `2b255e1` | Hand-rolled `StateGraph` (`agent`/`tools` nodes, conditional edge, `Annotated[list, add_messages]`), grew to 5 tools, `SqliteSaver` for durable persistence, `try/except` on every tool (later found to be over-applied), structured logging, Langfuse `CallbackHandler` | Control flow becomes an inspectable graph instead of a framework's hidden loop |
| 3 — Hardening pass 1 | `fa4350b` | Explicit `recursion_limit=10` + `GraphRecursionError` handling, deleted the dead `try/except` on 2 static tools, moved the 3 DB tools onto a pooled SQLAlchemy `engine`, caught `ollama.ResponseError`/`ConnectionError` after a real out-of-memory crash | Exception handling narrows to real boundaries instead of blanket coverage |
| 4 — Hardening pass 2 + eval | `1edd0c8` | 30-case parametrized `pytest` eval suite (`test_agent.py`) with hermetic `MemorySaver` + `uuid4` runs behind a `slow` marker; fixed `httpx.ConnectError` bypassing the existing except tuple on the streaming code path; anchored both DB paths to `Path(__file__).parent`; removed an unused leftover import | Verification becomes repeatable instead of manual, and a real transport-layer bug gets found and fixed |
**Current state, stated plainly:** a single-file (~173-line) CLI agent, five tools (three DB-backed,
two fixture data), one hardcoded `thread_id`, local-only inference, traced through a self-hosted
Langfuse stack, and checked by a 30-case suite that's now actually been run and scored: 28/30
correct on reconciliation, one confirmed reproducible tool-selection gap, one open question still
under-sampled — see war story #7 in Part 3 and Part 4 for the honest breakdown.
---
## Part 3 — War stories (interview-ready — know 3-4 cold)
1. **The stale interpreter.** A `NameError` kept firing against code that, on inspection, no
   longer had the bug. The file on disk was already fixed — the process actually executing was
   a stale one from an earlier run. *Lesson: when behavior doesn't match the code you're
   reading, check what's actually running before you re-check the code.*
2. **The exception-contract archaeology.** An `except (ResponseError, ConnectionError)` block
   looked complete — model-side errors and unreachable-server errors, both covered. Then a
   real connection-refused crash slipped through both. Reading the `ollama` client source
   (not guessing from the traceback) turned up two different code paths with two different
   exception contracts: `_request_raw()` catches `httpx.ConnectError` and re-raises it as a
   builtin `ConnectionError`; but `ChatOllama` uses the *streaming* path, whose `inner()`
   function opens `self._client.stream()` in a `with` block — if the TCP handshake fails at
   `__enter__`, `httpx.ConnectError` propagates raw, before any local `except` block in that
   function even runs. Same library, two paths, two contracts. Fix: add `httpx.ConnectError`
   to the tuple directly. *Lesson: a class hierarchy you haven't verified is a guess with good
   posture — the source is the only ground truth.*
3. **The real crash that exposed a real gap.** Hit
   `ollama._types.ResponseError: model requires more system memory (4.3 GiB) than is available
   (4.1 GiB)` mid-session. Checked actual memory instead of assuming: ~1 GB free out of 15.7 GB,
   with `vmmemWSL` — running the *entire* self-hosted Langfuse stack (Postgres, ClickHouse,
   Redis, MinIO, two Langfuse services) — holding 2.4+ GB continuously in the background for a
   tool used interactively maybe once an hour. Two separate fixes followed: an environment fix
   (stop the containers when not actively tracing) and a code fix the crash exposed (the invoke
   call only caught `GraphRecursionError`, so *any* other failure took the whole REPL down —
   added `ResponseError`/`ConnectionError` handling, scoped to what the `ollama` client
   actually raises). *Lesson: a resource crash is real signal about your environment, not just
   an exception to swallow.*
4. **The CWD-relative path bug.** Running the agent from inside the `langfuse-server/`
   subdirectory silently created brand-new, empty `agrios.db` and `checkpoints.db` files right
   there — no error, no warning, just a cold-start agent with no memory and an empty task
   table. Cause: `"sqlite:///agrios.db"` is a *process-CWD-relative* URI in SQLAlchemy (three
   slashes = relative path), so a string that reads like a fixed file path is actually resolved
   at runtime against wherever the shell happened to be when the process launched. Fixed by
   anchoring both connection strings to `Path(__file__).parent` — the directory `main.py`
   itself lives in, which cannot change no matter where the process is invoked from. *Lesson:
   anything that needs to persist across runs needs a path that's an explicit invariant in the
   code, not something inherited from the shell.*
5. **The try/except that guarded nothing.** Two of the five tools (`get_weather_forecast`,
   `lookup_crop_calendar`) had a `try/except Exception` wrapped around a hardcoded string
   return — no I/O, no real failure mode, nothing that block could ever actually catch. It had
   been added for consistency with the three DB tools, without asking what it was protecting
   against. The fix wasn't a narrower except clause — it was deleting the block entirely; a
   different-but-still-fake except would just be a different way of pretending there's a real
   failure mode there. The three DB tools kept their scoped `except SQLAlchemyError`, because
   those genuinely can fail. *Lesson: a try/except should mark a real boundary where failure is
   possible, not get copy-pasted onto every function out of habit.*
6. **Eval hermeticity.** The live app deliberately reuses `SqliteSaver` and a fixed
   `thread_id="1")` — that's what makes multi-turn memory durable. Reusing that exact setup for
   automated tests would have made test case N's outcome depend on whatever case N−1 left
   behind, since `SqliteSaver` persists by `thread_id` across invocations. The fix: tests
   compile the graph with an in-memory `MemorySaver()` once per session, and every call mints a
   fresh `uuid4()` thread — thirty isolated conversations sharing one compiled app object, none
   able to see another's history. *Lesson: the same feature that makes a system useful
   (persistent shared state) is exactly what makes testing it dangerous by default — hermetic
   tests have to actively opt out of the thing the product is for.*
7. **The eval suite's first real run, and a hypothesis that died on contact with a second test.**
   Ran the full 30-case suite for the first time: 27/30 on the first pass, 24m33s on CPU. One
   failure (`crop="potato"` vs. the model's `"potatoes"`) turned out to be the test's exact-match
   assertion being stricter than it needed to be — fixed by loosening it to the substring check
   already used elsewhere in the same file, the same "inspect before you blame the model" move as
   war story #2 and Agroteca's own q18. One failure (a recursion-limit hit on a frost-risk
   question) didn't reproduce on retry — one pass after one fail, not enough samples to call it
   either resolved or real, so it's a watch item, not a fix. The last one is the one worth
   remembering: `"What's the weather forecast for field-1?"` — wording that echoes the tool's own
   docstring almost verbatim — failed to trigger `get_weather_forecast` at all, while every
   paraphrase of the same request succeeded. First guess was a cold-start effect (it's the very
   first live model call of the session); tested that guess directly by rerunning the identical
   case alone, twice more, in fresh processes. Failed both times — **3 for 3**. Not a session
   artifact; a specific, reproducible phrasing gap. *Lesson: a hypothesis is worth having, but
   only worth keeping if it survives being tested on purpose — the same discipline Agroteca's
   embedder experiment ran on itself, just applied here in about two minutes instead of seven
   hours.*
---
## Part 4 — What's left (the honest roadmap)
**Not yet true, named plainly rather than implied:**
- **The eval suite has now been run and scored — 28/30, one confirmed gap.** See war story #7:
  a reproducible tool-selection miss on the phrasing closest to the weather tool's own docstring
  (`"What's the weather forecast for field-1?"`, 3/3 failures across independent runs), plus one
  under-sampled recursion-limit flake still being watched rather than fixed. The next honest step
  isn't running the suite again — it's fixing the one confirmed gap (a sharper docstring, or an
  explicit "always call this tool, never answer from memory" instruction) and re-verifying the
  same way it was found: one isolated case at a time, before trusting a full re-run.
- **The Postgres swap is a documented theory, not a tested fact.** SQLAlchemy makes the
  connection string the only SQLite-specific line in the file — but nobody has actually pointed
  the engine at a local Postgres instance to find out whether "one-line swap" holds up, or
  whether something about connection args, pooling behavior, or SQL dialect differences (e.g.
  `AUTOINCREMENT` is SQLite-specific; Postgres wants `SERIAL`/`IDENTITY`) turns out to need more.
  NOTES.md names this as the honest open question it is.
- **`get_weather_forecast` and `lookup_crop_calendar` return fixture data.** Both are shaped so
  a real API call could replace the body without touching the tool's interface — a deliberate
  seam, not yet used.
- **One hardcoded `thread_id`, no session handling.** Every conversation in a given run of the
  CLI shares the string `"1"`. Multi-user support means the `thread_id` has to come from
  *somewhere real* (a login, a session cookie, a CLI flag) instead of a literal.
- **"Streaming" today means the transport, not the output.** `ChatOllama` talks to the Ollama
  server over a streaming HTTP connection internally — that's the code path implicated in war
  story #2 — but `app.invoke()` still blocks until the full response is assembled, and the CLI
  prints once at the end. A user-facing token-by-token stream (the actual UX win Agroteca's
  Part 3 documents) hasn't been built here.
- **Tool outputs are strings, not structured data.** Every tool returns an f-string or
  `str(tuple(...))`/`str([...])` — which means the model has to *parse text back out of text*
  to use a result. A typed return (a small `dict`/Pydantic model, JSON-serialized) would remove
  that lossy round-trip.
- **No concurrency story.** Single process, single thread, one blocking `input()` loop. Serving
  more than one user at once is a different architecture, not a tuning knob.
None of this is a criticism of the project's current size — it's the difference between "what
this teaches" (real, already true) and "what this serves" (not yet attempted), stated the way
Agroteca's own roadmap insists on: a green checkmark only where there's a run to back it up.
---
## Part 5 — The Masterclass: the system, end to end
### 5.1 — The mental model: one process, one loop, one durable log
```
                                     ┌──────────────┐
                    START ─────────►│    agent     │◄────────────┐
                                     └──────┬───────┘             │
                                            │                     │
                              tool_calls present in the           │
                              model's last response?               │
                                     ┌──────┴───────┐              │
                                    yes             no              │
                                     │               │              │
                                     ▼               ▼              │
                              ┌───────────┐         END             │
                              │   tools   │──────────────────────────┘
                              └───────────┘
```
Every arrow above is a real Python function call, not a metaphor: `agent` is `agent_node()`,
`tools` is `tool_node()`, and the diamond is `should_continue()`. Unlike a prebuilt
`create_react_agent()` — which does the same thing but *inside* the library, invisibly — every
one of these is a function this project wrote and can step through in a debugger.
**Why it matters that this is a graph and not a `while` loop.** A `while` loop can express the
same control flow in five lines. What it can't express for free is: a checkpoint written after
every step (so the process can crash mid-loop and resume exactly where it left off), a trace
span emitted at every node boundary (so *which* step is slow or wrong is visible, not just the
final answer), and the ability to insert a new node later — a validation step, a human-approval
gate — without restructuring the whole function. The graph is the price you pay upfront for all
three later.
### 5.2 — The state, and the reducer that makes memory possible
```python
class AgentState(TypedDict):
    messages: Annotated[list, add_messages]
```
This one line is the most important line in the file, and it's worth being precise about what
it does. LangGraph doesn't treat a node's return value as a wholesale replacement of the state —
each key in the `TypedDict` is backed by a **channel**, and every time a node returns
`{"messages": [...]}`, that partial value is combined with whatever is already in the channel
by whatever function the channel is configured to use.
- **Without** the `Annotated[...]` wrapper, a key's default channel is "last write wins" —
  the new value *overwrites* the old one outright.
- **With** `Annotated[list, add_messages]`, the channel instead uses `add_messages` as its
  merge function: `add_messages(old_list, new_list)` appends the new messages onto the old
  ones rather than replacing them.
Concretely: `agent_node` returns `{"messages": [response]}` — a list containing exactly *one*
new `AIMessage`. If `messages` were an ordinary field, that single-item list would become the
*entire* conversation state after every turn — total amnesia, one AI message wide, with no
error ever raised to say so. The reducer is what turns "the model's newest reply" into
"the model's newest reply, appended to everything that came before."
**A detail worth knowing beyond "it appends":** `add_messages` also does ID-based upsert — if
an incoming message shares an `id` with a message already in the list, it *replaces* that
message in place instead of appending a duplicate. This is what lets a node emit an updated
version of a message it already produced (useful for streaming partial content into the same
message object) without special-casing it anywhere in this code — the reducer absorbs that
complexity so `agent_node` and `tool_node` don't have to know about it.
### 5.3 — Tool calling: from a Python function to a structured decision and back
```python
@tool
def query_field_status(field_id: str) -> str:
    """Returns the crop and status for a given field_id. ..."""
    ...
```
Four steps happen between writing that function and the model deciding to call it:
1. **Schema generation.** `@tool` introspects the function's signature and docstring and
   builds a schema for it — the function name becomes the tool name, the parameter names and
   type hints become a JSON Schema for the arguments, and the docstring becomes the
   natural-language description the model reads to decide *when* this tool is relevant.
2. **Binding.** `model.bind_tools(tools)` attaches the schemas for all five tools to every
   request sent to the model — the model doesn't call functions directly; it's shown a menu of
   available tools, described in structured text, on every turn.
3. **Structured emission.** For a model trained on tool-calling data (`qwen2.5:7b` here), the
   response isn't just prose — it can include a `tool_calls` list: structured objects naming
   which tool to call and with what arguments, parsed out of the model's raw output by the
   client library into `AIMessage.tool_calls`.
4. **Execution and feedback.** `tool_node` reads `last_message.tool_calls`, looks each one up
   in `tools_by_name`, actually runs the corresponding Python function, and wraps the result in
   a `ToolMessage` — critically, tagged with `tool_call_id`, the same ID the model's request
   carried, so when this gets appended back into `messages`, the model can see exactly which of
   its calls this result answers, even if it made several in the same turn.
The model never runs code. It only ever *proposes* a call, in a format the client can parse.
`tool_node` is the only thing in this whole system that actually executes anything — which is
precisely why every tool's failure mode is a Python exception this code controls, not something
the model can trigger directly.
### 5.4 — Checkpointing: what "the agent remembers" actually means
A **checkpoint** is a full snapshot of every channel's value, written after each node finishes
running (each such unit is a "super-step" in LangGraph's terms), keyed by `(thread_id,
checkpoint_id)`. `thread_id` is the partition key — every conversation lives under its own
`thread_id`, and passing the same `thread_id` again on a later call is what makes the agent
"remember" a previous turn: it isn't remembering in the sense of an LLM context window trick,
it's *reading its own prior state back out of storage* before it does anything else.
- **`MemorySaver`** stores checkpoints in a plain Python dict, living only as long as the
  process does. Simple, and exactly right for the eval suite, where "gone when the test session
  ends" is the desired behavior, not a limitation.
- **`SqliteSaver`** stores the same shape of data in a real SQLite table on disk
  (`checkpoints.db`). Same interface, durable storage — swapping one for the other required no
  change to `agent_node`, `tool_node`, or the graph definition at all, because both implement
  the same checkpointer interface. That's the entire point of a storage abstraction: what
  changes is *where the state lives*, not *what the state looks like* or *how the graph uses
  it*.
### 5.5 — Recursion limits: an agent loop has no built-in reason to stop
The `agent → tools → agent` cycle's only exit condition is `should_continue` seeing a response
with no `tool_calls` — and that's entirely a judgment the model makes on its own. Nothing
structural guarantees it happens. A smaller local model can misread a tool's result, decide it
needs to call the same tool again, and do that indefinitely. LangGraph counts super-steps
against a `recursion_limit` and raises `GraphRecursionError` once the count is exceeded —
this project sets that limit explicitly to `10` (tighter than LangGraph's own default of `25`),
specifically because a smaller model gets stuck faster than the library's default assumes, and
catches `GraphRecursionError` so a stuck loop degrades to a printed message instead of either
silently burning the full budget or crashing the whole REPL.
### 5.6 — Exception handling: three boundaries, nothing caught "just in case"
| Boundary | What's caught | Why exactly that |
|---|---|---|
| The agent/tools cycle itself | `GraphRecursionError` | The one structural failure mode of an unbounded model-driven loop |
| The model call (`ChatOllama`) | `ollama.ResponseError`, builtin `ConnectionError`, `httpx.ConnectError` | Confirmed by reading the `ollama`/`httpx` source: model-side errors, and *both* transport failure shapes across the streaming and non-streaming code paths |
| Each DB-backed tool | `SQLAlchemyError` | The only class of failure a SQL call against a real file can actually raise |
Nothing here is `except Exception`. Two static-data tools (`get_weather_forecast`,
`lookup_crop_calendar`) have **no** try/except at all, because there is currently nothing in
them that can fail — a `try/except` around code that can't throw doesn't protect anything, it
just hides the fact that nobody asked what it was for. The discipline is the same at every
layer: catch what you've *confirmed* can happen at a real boundary, and only that.
### 5.7 — Observability: two different tools, two different jobs
- **`logging`** (stdlib) — synchronous, local, ephemeral. `agent_node` and `tool_node` log
  message counts, whether tool calls were present, and which tool was called with what
  arguments, at the moment each happens. Good for watching a single run live in a terminal;
  gone the moment the process exits.
- **Langfuse `CallbackHandler`** — wired into the `invoke()` config's `callbacks` list, it
  intercepts each graph node's execution and the underlying model call, and ships structured
  spans (input, output, tool name/args/result, timing) to a self-hosted Langfuse stack running
  in Docker (Postgres + ClickHouse + Redis + MinIO + two Langfuse services). Durable, queryable,
  and — critically — the thing that closed the very first open question in this project's build
  log: being able to see *which* tool the model picked and *why*, not just the final printed
  answer.
### 5.8 — Testing: see Part 3, war story #6, and NOTES.md's newest entry
Covered in full detail there — the short version: 30 parametrized cases across four categories
(tool selection, argument correctness, task completion, no-tool-needed), run hermetically
against an in-memory checkpointer with a fresh thread per case, gated behind a `slow` marker so
the live-model cost is opt-in rather than automatic.
---
## Part 6 — Concepts from first principles
> Master this part and you can defend any design choice above at a whiteboard, in a system this
> size or one a hundred times larger — none of these ideas are specific to farming, Ollama, or
> even LangGraph.
### 6.1 — Graphs and state machines
A **graph** here means the formal object: a set of nodes (units of work) and edges (allowed
transitions between them). This project's graph is **cyclic**, not a DAG (directed *acyclic*
graph) — `tools` points back to `agent`, and that cycle is the entire mechanism of "the agent
keeps working until it's done." The reason to model an agent loop as a graph rather than plain
recursion or a `while` loop is that a graph gives the *runtime* a place to hook in extra
behavior — a checkpoint write, a trace span, a future validation node — at every node boundary,
uniformly, without touching the nodes' own code. A `while` loop has no such seams; every hook
has to be hand-inserted at every call site.
### 6.2 — The reducer pattern, formally
Every key in a LangGraph state schema is backed by a **channel**. The default channel type is
effectively "last value wins": a node's returned value for that key replaces whatever was
there. `Annotated[T, reducer_fn]` swaps in a channel that instead computes
`reducer_fn(existing_value, new_value)` and stores *that* as the new state. This is the same
shape of idea as a reducer in Redux, or an aggregation step in event sourcing: state isn't
"the last thing written," it's "the fold of everything ever written, through a function you
chose." `add_messages` is one such reducer, specialized for message lists (append, with
ID-based replace-in-place as the one exception to pure appending).
### 6.3 — The ReAct pattern, and what's explicit here that's usually implicit
"ReAct" (Yao et al., 2022) names the pattern of interleaving **Reasoning** (a thought about what
to do next), **Acting** (taking an action — here, a tool call), and **Observing** (reading the
result), repeated until the model decides it has enough to answer. Most agent frameworks
implement this as a hidden loop inside a single function call, with the "reasoning" folded
invisibly into the model's own generation. This project's `agent → tools → agent` cycle *is*
that loop — but made structural: each "act" is a real `tool_node()` invocation, each
"observe" is a real `ToolMessage` appended to state via the reducer, and the loop's exit
condition (`should_continue`) is a plain Python function you can read, not a prompt template
buried in a library.
### 6.4 — Tool calling as structured generation
A language model, left alone, produces free text. Tool calling is the technique of constraining
or steering that output into a *structured* shape — a schema-conformant object naming a
function and its arguments — so a client program can act on it mechanically instead of parsing
prose. The schema comes from the function's own signature and docstring (see 5.3); the model
was trained on data that teaches it to emit this structured form when a described tool fits the
user's request. The client library (`langchain-ollama` here) is responsible for recognizing
that structured output in the model's response and surfacing it as `AIMessage.tool_calls`
rather than as opaque text. The model proposes; the code around it — `tool_node`, in this
project — is the only thing that ever actually *does*.
### 6.5 — Checkpointing internals
A checkpoint is not a diff — it's a **full snapshot** of every channel's current value, written
after each super-step, addressed by `(thread_id, checkpoint_id)`. This is what makes two things
possible at once: **resumption** (load the latest checkpoint for a `thread_id` and continue as
if the process never stopped) and, in principle, **time travel** (a checkpointer can retain
every checkpoint in a thread's history, not just the latest, which is how some LangGraph
patterns implement "rewind and try a different branch" — not used in this project, but a direct
consequence of the same storage model). Swapping the *storage backend* (`MemorySaver` →
`SqliteSaver`) changes none of this mechanism — it only changes whether the snapshot survives
past the current process.
### 6.6 — Recursion limits as a fixed-point safety net
An agent/tools cycle is, structurally, an attempt to reach a fixed point: keep applying
(reason → act → observe) until reasoning stops requesting more action. Nothing guarantees that
fixed point is reached — the same way an arbitrary iterative numerical method isn't guaranteed
to converge. A recursion limit is the standard answer to "what if it doesn't": bound the number
of iterations and treat exceeding the bound as a distinguishable failure (`GraphRecursionError`)
rather than an infinite hang. The number itself (10 here, not the library default of 25) is a
judgment call about how many genuine back-and-forth tool exchanges a real task should need
versus how many iterations signal the model is stuck.
### 6.7 — Connection pooling
`QueuePool` maintains a bounded set of already-open database connections that get checked out
for a query and returned afterward, instead of opening (and authenticating, and tearing down) a
fresh connection every time. The textbook payoff is avoiding that handshake cost under
*concurrent* load. Stated honestly for this project: a single-threaded CLI processing one
request at a time isn't generating the contention pooling is designed to relieve — the actual
value captured here is different and more durable: the SQLAlchemy `engine`'s connection string
is now the *only* place in the file that knows it's talking to SQLite at all. Every tool
function speaks SQL through a portable API; moving to Postgres later is a one-line change to
that string, not a rewrite of five functions.
### 6.8 — Exception-boundary theory
The general principle demonstrated twice in this codebase (deleting a dead `try/except`;
finding the `httpx.ConnectError` gap): an exception handler should exist because a *specific,
confirmed* failure can happen at that exact point, not because "something might go wrong" in
the abstract. A blanket `except Exception` doesn't protect a program — it just hides which
failure mode nobody thought about, and silently converts a bug into a swallowed error instead
of a visible one. The antidote is empirical: read the actual code path (the library's source,
not its class hierarchy or its docs summary) to find out what it *actually* raises, then catch
exactly that.
### 6.9 — Path resolution: code invariants vs. environment state
A SQLAlchemy SQLite URL like `sqlite:///agrios.db` (three slashes) encodes a *relative* file
path, and a relative path is resolved against the process's current working directory —
a property of *how the process was launched*, set by the shell, not by the code. `Path(__file__)`
is different in kind: it resolves against where the *module itself* lives on disk, which cannot
change based on invocation. The general lesson: anything that must behave identically regardless
of how or from where a program is started needs to be anchored to something the code controls,
not something the environment happens to provide.
### 6.10 — Hermetic testing
A test is **hermetic** when its result depends only on its own inputs — never on what ran
before it, in what order, or in what environment. A system whose entire value proposition is
*persistent shared state* (this agent, via `SqliteSaver` + `thread_id`) is exactly the kind of
system where hermetic testing takes active, deliberate effort: the default way of running it
is the way that leaks state between calls. Restoring hermeticity here meant swapping the
*storage* (`MemorySaver`, gone at process end) and the *identity* (`uuid4()` per case, instead
of one shared literal) — while leaving every other piece of the graph completely untouched.
---
## Part 7 — System design
### 7.1 — Request lifecycle, end to end
```
input() reads a line
    → app.invoke({"messages": [("user", text)]}, config)
        → checkpointer loads prior state for thread_id (if any)
        → agent_node: model_with_tools.invoke(messages) → AIMessage (maybe with tool_calls)
        → checkpoint written (super-step 1)
        → should_continue: tool_calls present? → route to "tools" or END
        → [if tools] tool_node: execute each call via tools_by_name, build ToolMessage(s)
        → checkpoint written (super-step 2)
        → back to agent_node with the tool results appended
        → ... repeats until a response has no tool_calls, or recursion_limit is hit ...
    → result["messages"][-1].content printed
```
Every arrow that crosses a node boundary is also a place Langfuse's `CallbackHandler` and the
checkpointer both get a chance to act — tracing and persistence aren't bolted onto this flow,
they're a structural consequence of the flow being a graph made of discrete steps.
### 7.2 — The concurrency model, honestly
Today: one process, one thread, one blocking `input()` loop, one `thread_id` literal shared by
every conversation. There is no request queue, no async boundary, and the connection pool is
sized for headroom that a single-request-at-a-time CLI never actually uses. Serving more than
one user concurrently isn't a config change — it needs a request-scoped `thread_id` (derived
from a real session, not hardcoded), a non-blocking entrypoint instead of a REPL `input()` call,
and a pool sized against real concurrent load instead of an assumption.
### 7.3 — The observability path, concretely
A tool call becomes a visible trace through three hops: `tool_node` executes it and returns a
`ToolMessage`, the `CallbackHandler` attached to the `invoke()` config observes the graph
executing that step and serializes the relevant data (tool name, args, result, timing), and that
serialized span is shipped to the self-hosted Langfuse stack running in Docker — a Next.js web
app and a worker service, backed by Postgres (relational data), ClickHouse (trace/analytics
storage), Redis (queues/cache), and MinIO (S3-compatible blob storage). That's six containers
running for a single-user CLI's tracing needs — a real, named cost (see war story #3), not a
free abstraction.
### 7.4 — Local-inference tradeoffs, grounded in the actual incident
Running `qwen2.5:7b` locally means zero per-token cost and zero network dependency — but it also
means the model's ~4.3 GB RAM requirement is a hard, literal ceiling on a specific machine, one
that directly collided with the Langfuse Docker stack running in the background. "Free and
local" doesn't mean "no capacity planning" — it means the capacity planning is now about your
own machine's RAM instead of a cloud bill, and it's just as real when you hit the ceiling.
---
## Part 8 — Library choices, and the tradeoff space around them
### 8.1 — Agent control flow: `StateGraph` vs. the prebuilt constructors
The project's own git history *is* the evidence here: it started on `create_react_agent`
(LangGraph's prebuilt ReAct constructor), moved to `langchain.agents.create_agent` (a newer
prebuilt), and was ultimately rewritten as a hand-written `StateGraph` with explicit `agent`
and `tools` nodes. The prebuilt versions get you a working agent in two lines — at the cost of
the loop, the reducer, and the exit condition all living inside the library, invisible and
un-editable without reading its source. The explicit graph trades that convenience for every
piece of control flow being something this project's own code owns, can log, can checkpoint at
a chosen granularity, and can extend with a new node without waiting on an upstream API.
### 8.2 — Inference: local (Ollama) vs. a hosted API
**Chosen:** Ollama, running `qwen2.5:7b` fully offline.
**Pros:** zero marginal cost per call, no network dependency, no data (including the fixture
farm data) ever leaving the machine.
**Cons, made concrete by war story #3:** the model's RAM footprint is a real, named ceiling on a
constrained machine, tool-calling reliability on a 7B local model is meaningfully behind a
frontier hosted model, and there's no one else's infrastructure absorbing the failure modes —
every OOM, every dropped connection, is this project's problem to catch.
**The honest tradeoff:** a hosted API (OpenAI, Anthropic, etc.) would remove the RAM ceiling and
likely improve tool-selection accuracy, at the cost of per-call spend, a network dependency, and
data leaving the machine — a real consideration for a tool meant to eventually run useful
farm-operations data.
### 8.3 — Persistence: `MemorySaver` vs. `SqliteSaver` vs. a Postgres checkpointer
`MemorySaver` is the simplest checkpointer — a dict, gone at process exit — and it's exactly
right for two situations this project actually has: the very first prototype (before durability
mattered at all) and the eval suite (where *not* persisting across runs is the correct
behavior). `SqliteSaver` adds real durability while staying conceptually simple — still just a
local file, no new infrastructure. A Postgres-backed checkpointer is the natural next step for
anything multi-process or multi-user, and — per Part 4 — is a documented open question here,
not yet attempted, rather than a claim resting on nothing.
### 8.4 — Database access: SQLAlchemy vs. raw `sqlite3`
Raw `sqlite3` is what the project started with, and it's genuinely simpler for a single SQLite
file with no portability goal. SQLAlchemy's `engine` + `text()` earns its place here for one
specific, deliberate reason: it makes the connection string the single SQLite-specific fact in
the entire file. Every tool function already speaks parameterized SQL through an API that
doesn't care which database is on the other end of it — so the "one-line swap to Postgres" claim
in Part 4 is at least *structurally* set up to be true, even though it hasn't been tested yet.
### 8.5 — Observability: self-hosted Langfuse vs. LangSmith vs. nothing
Self-hosting Langfuse means running a real multi-container stack locally (Part 7.3) in exchange
for zero per-trace cost and no trace data leaving the machine — consistent with this project's
local-only stance everywhere else (the model, the database, the copyrighted-data-avoidance
non-issue that doesn't even apply here since there's no copyrighted corpus, but the *instinct*
is the same one Agroteca names explicitly: keep the sensitive layer on-device). LangSmith (or
any hosted tracing SaaS) would trade that setup cost for zero infrastructure to run yourself —
a real option for a project that didn't already have "everything stays local" as a working
principle everywhere else.
### 8.6 — Testing: `pytest` and its `mark`/`parametrize` machinery
`pytest.mark.parametrize` is what makes a 30-case matrix expressible as data (a list of tuples)
rather than 30 near-duplicate test functions, and `pytest.mark.slow` + `pytest.ini`'s
`addopts = -m "not slow"` is what makes the fast/live split a one-flag decision instead of a
separate test runner or a manual skip. `unittest` (stdlib) can express the same tests, but with
more boilerplate per case and without a first-class marker system for the fast/slow split this
project actually needed.
---
## Part 9 — Interview question bank
- **Walk me through the architecture.** → *A `StateGraph` with two nodes — `agent`, which calls
  the model with the running message history and its bound tools, and `tools`, which executes
  whichever tools the model requested. A conditional edge checks the model's last response for
  `tool_calls`: present, route back to `tools`; absent, exit to `END`. State persists per
  `thread_id` via a checkpointer, so the loop and the memory are two separately swappable
  pieces.*
- **Why a graph instead of a `while` loop?** → *A `while` loop can express the same control
  flow, but a graph gives the runtime a uniform place to hook in a checkpoint write and a trace
  span at every node boundary, and a place to add a new node later without restructuring
  anything. Those seams don't exist for free in a hand-rolled loop.*
- **What's a reducer, and why did you need one?** → *State updates in LangGraph merge through
  whatever function a field's channel is configured with — the default is overwrite.
  `Annotated[list, add_messages]` swaps that for append-with-ID-based-replace. Without it, each
  node's single-message return would *replace* the whole conversation instead of extending it —
  silent amnesia, no error raised.*
- **How does checkpointing actually work?** → *A checkpoint is a full snapshot of every state
  channel, written after each node executes, keyed by `(thread_id, checkpoint_id)`. Passing the
  same `thread_id` again loads that snapshot before doing anything else — that's the entire
  mechanism behind "the agent remembers." `MemorySaver` and `SqliteSaver` implement the same
  interface over different storage, so swapping one for the other touched zero lines in the
  graph or the nodes.*
- **Why an explicit recursion limit?** → *The agent/tools cycle's exit condition is entirely a
  model judgment call — nothing structurally guarantees the loop terminates. I was relying on
  LangGraph's default of 25 without ever having decided that number was right for a 7B local
  model that can misread a tool result and re-call it. Set it to 10 explicitly, and catch
  `GraphRecursionError` so a stuck loop degrades to a message instead of crashing or silently
  burning the whole budget.*
- **How do you decide what to catch?** → *At a boundary where a specific, confirmed failure can
  happen — never `except Exception` speculatively. I deleted a `try/except` that wrapped code
  with no real failure mode, and separately found (by reading the `ollama` client's source, not
  guessing) that its streaming and non-streaming code paths raise different exception types for
  the same underlying failure — the fix was adding exactly the one type that was missing, not
  widening the catch.*
- **Tell me about a bug you're proud of finding.** → *An `except (ResponseError,
  ConnectionError)` block looked complete but a real connection-refused crash slipped through
  it. `ChatOllama` uses `ollama`'s streaming code path, which — unlike the non-streaming path —
  doesn't wrap `httpx.ConnectError` before it propagates. Found by reading the library's source
  at the exact function where the TCP handshake happens, not by guessing from the traceback.*
- **Why hermetic tests for something that's supposed to remember things?** → *Because the thing
  that makes the product useful — persistent state keyed by `thread_id` — is exactly what
  breaks test independence if you reuse it as-is. Tests use an in-memory checkpointer and a
  fresh random thread per case specifically to opt out of the feature the rest of the system is
  built around.*
- **Why gate the eval suite behind a marker instead of just running it?** → *It calls a live
  local model — slow, environment-dependent (model has to be pulled, server has to be running),
  and non-deterministic by nature. A default `pytest` run should stay fast and dependency-free;
  the full suite should be one flag away, not a tax on every casual check.*
- **What would you change to make this serve more than one user?** → *The `thread_id` needs to
  come from a real session instead of a literal `"1"`, the blocking `input()` REPL needs to
  become a non-blocking entrypoint, and the connection pool needs to be sized against actual
  concurrent load instead of headroom nothing currently uses.*
- **Why local inference?** → *Zero marginal cost, works offline, no data leaving the machine —
  consistent with treating this as a tool meant to eventually run on a real farm's own data. The
  honest cost, which I've actually hit: a hard RAM ceiling that collided with other things
  running on the same machine, and tool-calling reliability behind what a frontier hosted model
  would give you.*
- **What surprised you building this?** → *That the two hardest bugs — the exception-contract
  gap and the CWD-relative path — were both cases where the code looked obviously correct and
  the actual failure lived one layer down, in a library's internals or in an environment
  assumption neither the code nor a casual read would surface. Both needed reading source, not
  re-reading my own file harder.*
- **Tell me about a time you tested your own hypothesis, not just your code.** → *My eval
  suite's first real run failed a case I expected to be trivial. My first explanation was a
  cold-start effect — it was the first live model call of the session. Instead of writing that
  down, I reran the identical case alone, twice more, in fresh processes. It failed both times.
  That killed the cold-start theory and pointed at something more specific and more useful: a
  reproducible phrasing gap, not a session artifact — the exact wording closest to the tool's own
  docstring was the one case the model answered from memory instead of calling the tool. The
  instinct to verify a plausible-sounding explanation instead of shipping it is the same one that
  caught Agroteca's disproved embedder hypothesis — just far cheaper to run here.*
---
## Part 10 — Concepts to master (the syllabus)
> The checklist to own this subject generally, not just this repo.
**Agent architectures**
- The ReAct pattern (reason / act / observe) and why most frameworks hide it inside a
  black-box loop — *the pattern this project made structurally explicit.*
- Graphs vs. plain recursion/loops for control flow — *what a graph buys you at the seams.*
- Prebuilt agent constructors vs. hand-rolled graphs — *convenience vs. inspectability, a real
  tradeoff this project's own history walked through.*
**State & persistence**
- Functional state updates / reducers (the Redux-style mental model) — *why "last write wins"
  isn't always the right default.*
- Checkpointing: snapshots, partition keys, storage-backend swaps behind one interface.
- The distinction between "what state looks like" and "where state lives" — *the whole reason
  `MemorySaver` → `SqliteSaver` was a one-line change.*
**LLM tool calling**
- Schema generation from a function signature + docstring.
- Structured generation vs. free text — *tool calling as a constrained-output problem.*
- The model proposes, the client executes — *never conflate "the model called a function" with
  what actually happened.*
**Bounded execution & failure**
- Recursion/iteration limits as a fixed-point safety net for a process with no guaranteed
  termination.
- Exception-boundary discipline: catch what's confirmed, at the boundary where it's confirmed.
- Reading library source over trusting a class hierarchy or a stack trace's first guess.
**Observability**
- Structured logging vs. distributed tracing — *different tools, different lifetimes, different
  jobs.*
- Callback/hook patterns for instrumenting a system without modifying its core logic.
**Databases**
- Connection pooling: what it solves, and being honest about whether your current scale needs
  it — *vs. the abstraction-boundary value that's real regardless of scale.*
- Relative vs. anchored file paths — *an environment property vs. a code invariant.*
**Testing & eval**
- Hermetic testing — *why shared/persistent state is dangerous for test independence by
  default, and what "opt out of the product's own feature" looks like in test code.*
- Parametrized testing across categorized failure modes, not just one happy-path check.
- Gating slow/live/non-deterministic tests behind an opt-in marker.
**The frontiers to grow into**
- Actually running the eval suite and scoring it — *the very next step this project hasn't
  taken yet.*
- Testing the Postgres swap for real, not just structuring code to make it plausible.
- Structured (typed) tool outputs instead of stringified data.
- Token-level streaming to a user-facing surface, not just the internal transport.
- Multi-user session handling — *a real `thread_id` source instead of a literal.*
- Human-in-the-loop / interrupt patterns — *a natural next node type, given the graph is
  already built to add one.*
> **How to use this part:** for each unfamiliar line, write a one-paragraph note in your own
> words, then find or write the smallest possible experiment that proves it. That's the same
> loop that built the rest of this project — writing the fix is how you find out whether you
> actually understood the bug.
