from langchain_ollama import ChatOllama
from langchain.agents import create_agent
from langchain_core.tools import tool
from langgraph.checkpoint.memory import MemorySaver

@tool
def get_weather_forecast(location: str, days:int=7) -> str:
    """Returns the daily weather forecast for a farm location over the
    next N days. Use when the user asks about planting, irrigation,
    frost risk, or any weather-dependent timing."""
    return '{"location": "%s", "forecast": [{"day": 1, "temp_c": 14, "rain_mm": 2}, {"day": 2, "temp_c": 11, "rain_mm": 8}]}' % location

model = ChatOllama(model="qwen2.5:7b")
memory = MemorySaver()
agent = create_agent(model, [get_weather_forecast], checkpointer=memory)
config = {"configurable": {"thread_id": "1"}}

while True:
    user_input = input("You: ")
    if user_input.lower() == "exit":
        break

    result = agent.invoke(
        {"messages": [("user", user_input)]},
        config
    )
    print(result["messages"][-1].content)