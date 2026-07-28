#ifndef OPENAI_REALTIME_API_H
#define OPENAI_REALTIME_API_H

#include <Arduino.h>

namespace openai_realtime {

typedef size_t (*MicReadFn)(int16_t* pcmBuf, size_t maxSamples);
typedef bool (*StopFn)();

// Per-call latency and throughput data. The legacy fields intentionally match
// deepgram::Timing so existing serial-log tooling can continue to parse them.
struct Timing {
    unsigned long encodeMs        = 0;
    unsigned long uploadMs        = 0;
    unsigned long firstResponseMs = 0; // First audio relative to audio-send start
    unsigned long lastResponseMs  = 0; // Final response relative to audio-send start
    unsigned long decodeMs        = 0;
    size_t        uploadBytes     = 0; // Raw PCMU bytes (before Base64/JSON framing)
    size_t        downloadBytes   = 0; // Raw PCMU bytes (after Base64 decoding)

    unsigned long connectMs       = 0; // TLS dial + WebSocket handshake
    unsigned long sessionSetupMs  = 0; // session.created -> session.updated
    unsigned long responseWaitMs  = 0; // response.create -> first output audio
    unsigned long totalMs         = 0; // Encode through output decode/cleanup
    size_t        wireUploadBytes = 0; // WebSocket bytes sent for audio append events
    unsigned long inputTokens     = 0;
    unsigned long outputTokens    = 0;
};

// Send a WAV file through OpenAI Realtime and return a PSRAM-allocated PCMU
// WAV response. The caller owns *outWav and must free() it.
bool audioToAudio(
    const char* audioPath,
    uint8_t**   outWav,
    size_t*     outWavLen,
    const char* prompt,
    Timing*     outTiming      = nullptr,
    bool        printBreakdown = true
);

// Stream 16 kHz PCM16 microphone samples to OpenAI Realtime. The returned
// buffer is decoded PCM16 at 8 kHz and must be freed by the caller.
bool liveToAudio(
    MicReadFn   micReadFn,
    StopFn      stopFn,
    uint8_t**   outPcm16,
    size_t*     outPcm16Len,
    const char* prompt
);

} // namespace openai_realtime

#endif
