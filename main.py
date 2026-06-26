from langchain_ollama import ChatOllama
from langchain.agents import create_agent
from langchain_core.tools import tool
from langgraph.checkpoint.memory import MemorySaver
from langgraph.graph.message import add_messages
from langgraph.graph import StateGraph, START
from langgraph.graph import END
from langchain_core.messages import ToolMessage
from typing import TypedDict, Annotated
from langgraph.checkpoint.sqlite import SqliteSaver
from dotenv import load_dotenv
from langfuse.langchain import CallbackHandler
import sqlite3
import logging

load_dotenv()
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
logger = logging.getLogger(__name__)


def init_db():
    conn = sqlite3.connect("agrios.db")
    conn.execute("""
        CREATE TABLE IF NOT EXISTS fields (
            field_id TEXT PRIMARY KEY,
            crop TEXT,
            status TEXT
        )
    """)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS tasks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            field_id TEXT,
            description TEXT,
            status TEXT DEFAULT 'pending'
        )
    """)
    if conn.execute("SELECT COUNT(*) FROM fields").fetchone()[0] == 0:
        conn.executemany(
            "INSERT INTO fields (field_id, crop, status) VALUES (?, ?, ?)",
            [("field-1", "potato", "growing"), ("field-2", "wheat", "fallow")]
        )
        conn.commit()
    conn.close()

class AgentState(TypedDict):
    messages: Annotated[list, add_messages]

def agent_node(state: AgentState) -> dict:
    logger.info("agent_node: %d messages in", len(state["messages"]))
    response = model_with_tools.invoke(state["messages"])
    logger.info("agent_node: tool_calls=%s", bool(response.tool_calls))
    return {"messages": [response]}

def should_continue(state: AgentState):
    last_message = state["messages"][-1]
    if last_message.tool_calls:
        return "tools"
    return END


init_db()

def tool_node(state: AgentState):
    last_message = state["messages"][-1]
    results = []
    for call in last_message.tool_calls:
        logger.info("tool_node: calling %s(%s)", call["name"], call["args"])
        tool = tools_by_name[call["name"]]
        output = tool.invoke(call["args"])
        results.append(
            ToolMessage(content=str(output), tool_call_id=call["id"], name=call["name"])
        )
    return {"messages": results}
@tool
def get_weather_forecast(location: str, days:int=7) -> str:
    """Returns the daily weather forecast for a farm location over the
    next N days. Use when the user asks about planting, irrigation,
    frost risk, or any weather-dependent timing."""
    try: 
     return '{"location": "%s", "forecast": [{"day": 1, "temp_c": 14, "rain_mm": 2}, {"day": 2, "temp_c": 11, "rain_mm": 8}]}' % location
    except Exception as e:
        logger.exception("get_weather_forecast failed for %s", location)
        return f"Error getting weather forecast for {location}: {e}"

@tool
def lookup_crop_calendar(crop: str, region: str) -> str:
    """Returns planting and harvest windows for a crop in a given region.
    Use when the user asks when to plant or harvest a specific crop."""
    try:
        return '{"crop": "%s", "region": "%s", "planting_window": {"start": "2024-04-01", "end": "2024-05-15"}, "harvest_window": {"start": "2024-09-01", "end": "2024-10-15"}}' % (crop, region)
    except Exception as e:
        logger.exception("lookup_crop_calendar failed for %s in %s", crop, region)
        return f"Error looking up crop calendar for {crop} in {region}: {e}"

@tool
def query_field_status(field_id: str) -> str:
    """Returns the crop and status for a given field_id. field_id must match
the format "field-<n>", e.g. "field-1" or "field-2"."""
    try:
        conn = sqlite3.connect("agrios.db")
        row = conn.execute(
            "SELECT field_id, crop, status FROM fields WHERE field_id = ?", (field_id,)
        ).fetchone()
        conn.close()
        return str(row) if row else f"No field found with id {field_id}"
    except sqlite3.Error as e:
        logger.exception("query_field_status failed for %s", field_id)
        return f"Error looking up field {field_id}: {e}"


@tool
def log_task(field_id: str, description: str) -> str:
    """Logs a pending task for a field. Use when the user wants to record
    a future action like irrigating, fertilizing, or inspecting for a given field_id. field_id must match
the format "field-<n>", e.g. "field-1" or "field-2"."""
    try:
        conn = sqlite3.connect("agrios.db")
        row = conn.execute(
            "INSERT INTO tasks (field_id, description, status) VALUES (?, ?, 'pending')",
            (field_id, description)
        )
        conn.commit()
        conn.close()
        return f"Logged task for {field_id}: {description}"
    except sqlite3.Error as e:
        logger.exception("log_task failed for %s", field_id)
        return f"Error logging task for {field_id}: {e}"

@tool
def get_pending_tasks() -> str:
    """Returns all tasks currently pending across all fields."""
    try:
        conn = sqlite3.connect("agrios.db")
        rows = conn.execute(
            "SELECT field_id, description FROM tasks WHERE status = 'pending'"
        ).fetchall()
        conn.close()
        return str(rows)
    except sqlite3.Error as e:
        logger.exception("get_pending_tasks failed")
        return f"Error retrieving pending tasks: {e}"

model = ChatOllama(model="qwen2.5:7b")
config = {"configurable": {"thread_id": "1"}}
langfuse_handler = CallbackHandler()
tools = [get_weather_forecast, lookup_crop_calendar, query_field_status, log_task, get_pending_tasks]
model_with_tools = model.bind_tools(tools)
tools_by_name = {t.name: t for t in tools}
graph = StateGraph(AgentState)
graph.add_node("agent", agent_node)
graph.add_node("tools", tool_node)

graph.add_edge(START, "agent")
graph.add_conditional_edges("agent", should_continue, {"tools": "tools", END: END})
graph.add_edge("tools", "agent")

with SqliteSaver.from_conn_string("checkpoints.db") as memory:
    app = graph.compile(checkpointer=memory)
    while True:
        user_input = input("You: ")
        if user_input.lower() == "exit":
            break

        result = app.invoke(
            {"messages": [("user", user_input)]},
            config={**config, "callbacks": [langfuse_handler]}
        )
        print(result["messages"][-1].content)