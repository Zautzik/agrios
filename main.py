from langchain_ollama import ChatOllama
from langgraph.prebuilt import create_react_agent
from langchain_core.tools import tool

@tool
def get_weather_forecast(location: str, days:int=7) -> str:
    """Returns the daily weather forecast for a farm location over the
    next N days. Use when the user asks about planting, irrigation,
    frost risk, or any weather-dependent timing."""
    return '{"location": "%s", "forecast": [{"day": 1, "temp_c": 14, "rain_mm": 2}, {"day": 2, "temp_c": 11, "rain_mm": 8}]}' % location

model = ChatOllama(model="qwen2.5:7b")
agent = create_react_agent(model, [get_weather_forecast])

result = agent.invoke({"messages":[("user","Should I irrigate this week in Temuco?")]})
print(result["messages"][-1].content)