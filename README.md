# Walter Board - 5G AI Glasses

## Where We're At

Currently the board is capable of sending and recieving audio data from Gemini and Deepgrams' API. Deepgram is the best because it offers more versatile audio compression options which can drastically reduce the latency in the response. I coded it so that each API has its own class separated into difference header and source files so you should be able to copy and paste them into any program where you want to use the cellular capabilities. Usage examles for each are below.

**Deepgram**

The Deepgram class uses the Voice Agent WebSocket API. It handles the full pipeline: audio encoding (PCM→µ-law), WebSocket connection/handshake, settings negotiation, streaming upload with interleaved response collection, and decoding. ChatGPT (`gpt-4o-mini`) is used internally by Deepgram as the inference model.

`audioToAudio` — send a WAV file from the filesystem, get a WAV response back:
```cpp
#include "deepgram_api.h"

// Requires audioFsInit() and modemInit() to be called first

uint8_t* wav = nullptr;
size_t   wavLen = 0;

bool ok = deepgram::audioToAudio(
    "/msg_tiny.wav",       // path to WAV on FFat filesystem
    &wav, &wavLen,         // PSRAM-allocated output WAV (caller must free)
    "You are a helpful voice assistant. Be concise.",  // agent prompt
    "aura-2-asteria-en"    // Deepgram TTS voice
);

if (ok && wav) {
    // wav contains a µ-law WAV at 8kHz — play via I2S, dump to serial, etc.
    free(wav);
}

// Optional: pass a Timing struct to get per-phase latency data
deepgram::Timing timing;
deepgram::audioToAudio("/msg_tiny.wav", &wav, &wavLen,
                       "Be concise.", "aura-2-asteria-en",
                       &timing, /*printBreakdown=*/false);
// timing.encodeMs, timing.uploadMs, timing.firstResponseMs, etc.
```

`liveToAudio` — stream live mic audio, get PCM16 response back:
```cpp
#include "deepgram_api.h"

// Define callbacks for mic input and stop condition
size_t myMicRead(int16_t* pcmBuf, size_t maxSamples) {
    // Read up to maxSamples 16-bit PCM samples from your mic (e.g. via I2S)
    // Return the number of samples actually read
}

bool myStopCheck() {
    // Return true when the user wants to stop recording (e.g. button press)
}

uint8_t* pcm16    = nullptr;
size_t   pcm16Len = 0;

bool ok = deepgram::liveToAudio(
    myMicRead, myStopCheck,
    &pcm16, &pcm16Len,    // PSRAM-allocated PCM16 @ 8kHz (caller must free)
    "You are a helpful voice assistant. Be concise.",
    "aura-2-asteria-en"
);

if (ok && pcm16) {
    // pcm16 contains signed 16-bit PCM at 8kHz — play via I2S
    free(pcm16);
}
```

**Gemini**

The Gemini class wraps two separate REST endpoints: one for speech-to-text (audio→text inference) and one for text-to-speech. Chain them together for a full audio→text→audio pipeline.

`audioToText` — send a WAV file, get transcription/inference text back:
```cpp
#include "gemini_api.h"

// Requires modemInit() to be called first

String response = gemini::audioToText(
    "/msg_tiny.wav",         // path to WAV on FFat filesystem
    "Transcribe this audio." // prompt sent alongside the audio
);

if (!response.isEmpty()) {
    Serial.println(response);
}
```

`textToAudio` — send text, get a WAV response back:
```cpp
#include "gemini_api.h"

uint8_t* wav = nullptr;
size_t   wavLen = 0;

bool ok = gemini::textToAudio(
    "Hello, how can I help you today?",  // text to synthesize
    &wav, &wavLen                        // PSRAM-allocated WAV (caller must free)
);

if (ok && wav) {
    // wav contains PCM audio — play via I2S, etc.
    free(wav);
}
```

Full STT → inference → TTS pipeline:
```cpp
String text = gemini::audioToText("/input.wav", "Transcribe this audio.");
if (!text.isEmpty()) {
    uint8_t* wav = nullptr;
    size_t   wavLen = 0;
    if (gemini::textToAudio(text.c_str(), &wav, &wavLen)) {
        // Play wav via I2S speaker...
        free(wav);
    }
}
```



The current issue preventing a full working prototype of an agent system is in hardware. I struggled to get the speaker working; its output is extremely noisy and quiet. The microphone is working properly though. I was unable to determine whether this is an issue with the amp board I was using, a simple hardware disconnection somewhere, or my poor soldering job. 

## Discovered Limitations

A lot of the work centered around working around the limitations of the ESP32's connection with the on board modem. When sending and receiving data, we're limited not by the boards cellular connection but by the data transfer rate between the board and the modem. Websockets work via rings. The max amount of data that can be transferred per ring is 1500bytes. Initially the software implementation just pulled rings when any data was detected but this was slow because of ring overhead. Now the software chunks data and only pulls when enough data is present in the modem so that it is at or near the 1500byte limit. Currently, the bandwidth is limited by this 1500byte limit + the modem overhead. 



An additional issue was encountered with the Walter Board's micropython library. When rapidly pulling rings it will sometimes concatenated the AT commands into the data leading the board to crash because it can't parse the data. I was able to implement a fix for this but we should avoid using micropython on the Walter because the library isn't as well developed as their Arduino/ESP-IDF package.