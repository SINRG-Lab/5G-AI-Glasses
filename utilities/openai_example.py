import json
import websocket
from pathlib import Path

config_path = Path(__file__).parent.parent / "spiffs_data" / "config.json"

with open(config_path, "r") as f:
    config = json.load(f)
    OPENAI_API_KEY = config["openai"]["api_key"]

url = "wss://api.openai.com/v1/realtime?model=gpt-realtime"
headers = ["Authorization: Bearer " + OPENAI_API_KEY]


def on_open(ws):
    print("Connected to server.")


def on_message(ws, message):
    data = json.loads(message)
    print("Received event:", json.dumps(data, indent=2))


ws = websocket.WebSocketApp(
    url,
    header=headers,
    on_open=on_open,
    on_message=on_message,
)

ws.run_forever()