import uuid
import pytest
from main import graph
from langgraph.checkpoint.memory import MemorySaver

@pytest.fixture(scope="session")
def app():
    return graph.compile(checkpointer=MemorySaver())

def run_agent(app, user_input: str):
    config = {
        "configurable": {"thread_id": str(uuid.uuid4())},
        "recursion_limit": 10,
    }
    result = app.invoke({"messages": [("user", user_input)]}, config=config)
    tool_calls = [
        call
        for msg in result["messages"]
        for call in (getattr(msg, "tool_calls", None) or [])
    ]
    return result, tool_calls

TOOL_SELECTION_CASES = [
    ("What's the weather forecast for field-1?",          "get_weather_forecast"),
    ("Will it rain in field-2 this week?",                "get_weather_forecast"),
    ("Is frost a risk for field-1 in the next 7 days?",   "get_weather_forecast"),
    ("When should I plant potatoes in central Chile?",     "lookup_crop_calendar"),
    ("What's the harvest window for wheat in Chile?",      "lookup_crop_calendar"),
    ("When can I harvest the potatoes I'm growing?",       "lookup_crop_calendar"),
    ("What's the status of field-1?",                     "query_field_status"),
    ("What crop is growing in field-2?",                  "query_field_status"),
    ("Is field-1 fallow or growing something right now?", "query_field_status"),
    ("Log a task to fertilize field-1",                   "log_task"),
    ("Remind me to inspect field-2 next week",            "log_task"),
    ("I need to irrigate field-1, log that",              "log_task"),
    ("What tasks are still pending?",                     "get_pending_tasks"),
    ("Show me everything that hasn't been done yet",      "get_pending_tasks"),
    ("List my open to-dos across all fields",             "get_pending_tasks"),
]

@pytest.mark.parametrize(
    "user_input,expected_tool",
    TOOL_SELECTION_CASES,
    ids=[c[0][:45] for c in TOOL_SELECTION_CASES],
)
def test_tool_selection(app, user_input, expected_tool):
    _, tool_calls = run_agent(app, user_input)
    called = {c["name"] for c in tool_calls}
    assert expected_tool in called, (
        f"expected {expected_tool!r}, model called {called or 'nothing'}"
    )

ARGUMENT_CASES = [
    ("Log an irrigation task for field-2",           "log_task",             {"field_id": "field-2"}, {"description": "irrigat"}),
    ("Field-1 needs fertilizing, log it",            "log_task",             {"field_id": "field-1"}, {"description": "fertiliz"}),
    ("What's growing in field-2?",                   "query_field_status",   {"field_id": "field-2"}, {}),
    ("Check the status of field-1",                  "query_field_status",   {"field_id": "field-1"}, {}),
    ("Weather for field-2 please",                   "get_weather_forecast", {"location": "field-2"}, {}),
    ("Forecast for field-1 over the next week",      "get_weather_forecast", {"location": "field-1"}, {}),
    ("When to plant wheat in the central valley",    "lookup_crop_calendar", {"crop": "wheat"},        {}),
    ("Harvest window for potatoes in Chile",         "lookup_crop_calendar", {"crop": "potato"},       {}),
]

@pytest.mark.parametrize(
    "user_input,expected_tool,exact_args,substr_args",
    ARGUMENT_CASES,
    ids=[c[0][:45] for c in ARGUMENT_CASES],
)
def test_argument_correctness(app, user_input, expected_tool, exact_args, substr_args):
    _, tool_calls = run_agent(app, user_input)
    matches = [c for c in tool_calls if c["name"] == expected_tool]
    assert matches, (
        f"expected a call to {expected_tool!r}, "
        f"model called {[c['name'] for c in tool_calls] or 'nothing'}"
    )
    args = matches[0]["args"]
    for key, expected_val in exact_args.items():
        assert args.get(key) == expected_val, (
            f"{key}: expected {expected_val!r}, got {args.get(key)!r}"
        )
    for key, substr in substr_args.items():
        actual = str(args.get(key, "")).lower()
        assert substr.lower() in actual, (
            f"{key}: expected to contain {substr!r}, got {actual!r}"
        )
TASK_COMPLETION_CASES = [
    ("Check field-1's status and log a task to fertilize it",           {"query_field_status", "log_task"}),
    ("Look up field-2 and log an irrigation task for it",               {"query_field_status", "log_task"}),
    ("Tell me what's pending, then log a new task to inspect field-1",  {"get_pending_tasks",  "log_task"}),
    ("Check the weather for field-1 and log irrigation if it looks dry",{"get_weather_forecast"}),
    ("What's in field-2 and when should I harvest per the crop calendar",{"query_field_status", "lookup_crop_calendar"}),
]

@pytest.mark.parametrize(
    "user_input,required_tools",
    TASK_COMPLETION_CASES,
    ids=[c[0][:45] for c in TASK_COMPLETION_CASES],
)
def test_task_completion(app, user_input, required_tools):
    _, tool_calls = run_agent(app, user_input)
    called = {c["name"] for c in tool_calls}
    assert required_tools <= called, (
        f"expected at least {required_tools}, model only called {called or 'nothing'}"
    )


NO_TOOL_CASES = [
    "What can you help me with?",
    "Hi, who are you?",
]

@pytest.mark.parametrize("user_input", NO_TOOL_CASES)
def test_no_tool_needed(app, user_input):
    _, tool_calls = run_agent(app, user_input)
    assert tool_calls == [], (
        f"expected no tool calls, model called {[c['name'] for c in tool_calls]}"
    )
pytestmark = pytest.mark.slow
