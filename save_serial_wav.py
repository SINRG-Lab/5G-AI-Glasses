"""
Helper file for downloading wav data that is dumped over the serial line.
"""
import base64

with open('serial_log.txt', encoding='utf-8') as f:
    lines = f.readlines()

in_block = False
b64 = []
for line in lines:
    if '---BEGIN WAV BASE64---' in line:
        in_block = True
    elif '---END WAV BASE64---' in line:
        break
    elif in_block:
        b64.append(line.strip())

wav = base64.b64decode(''.join(b64))
open('tts_output.wav', 'wb').write(wav)
print(f"Saved {len(wav)} bytes")