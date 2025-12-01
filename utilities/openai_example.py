import json
import websocket
from pathlib import Path
from datetime import datetime

config_path = Path(__file__).parent.parent / "spiffs_data" / "config.json"

with open(config_path, "r") as f:
    config = json.load(f)
    OPENAI_API_KEY = config["openai"]["api_key"]

url = "wss://api.openai.com/v1/realtime?model=gpt-realtime"
headers = ["Authorization: Bearer " + OPENAI_API_KEY]

log_file = "utilities/websocket_traffic.log"

def log_event(direction, data):
    timestamp = datetime.now().isoformat()
    with open(log_file, "a") as f:
        f.write(f"\n{'=' * 60}\n")
        f.write(f"{direction} at {timestamp}\n")
        if isinstance(data, str):
            f.write(data)
        else:
            f.write(json.dumps(data, indent=2))
        f.write(f"\n{'=' * 60}\n")

def on_open(ws):
    print("Connected to server.")
    log_event("CONNECTION", {"status": "opened"})

    # response.create
    test_message = {
        "type": "conversation.item.create",
        "item": {
            "type": "message",
            "role": "user",
            "content": [
                {
                    "type": "input_text",
                    "text": "Say 'Hello, world!'"
                }
            ]
        }
    }
    ws.send(json.dumps(test_message))

    # Then trigger a response
    ws.send(json.dumps({"type": "response.create"}))

def on_message(ws, message):
    data = json.loads(message)
    print("Received event:", json.dumps(data, indent=2))
    log_event("RECEIVED", data)

def logged_send(data, opcode=websocket.ABNF.OPCODE_TEXT):
    print(f"Sending: {data}")
    try:
        parsed_data = json.loads(data)
        log_event("SENT", parsed_data)
    except json.JSONDecodeError:
        log_event("SENT (binary/non-JSON)", data)
    return original_send(data, opcode)

# Create the WebSocket app
ws = websocket.WebSocketApp(
    url,
    header=headers,
    on_open=on_open,
    on_message=on_message,
)

# Monkey-patch the send method to log outbound messages
original_send = ws.send

ws.send = logged_send

ws.run_forever()