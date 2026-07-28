#ifndef DEEPGRAM_API_H
#define DEEPGRAM_API_H

#include <Arduino.h>

namespace deepgram {

// Callback: fills pcmBuf with up to maxSamples int16 samples from the mic.
// Returns the number of samples actually read (0 on timeout).
typedef size_t (*MicReadFn)(int16_t* pcmBuf, size_t maxSamples);

// Callback: returns true when recording should stop (e.g. button pressed).
typedef bool (*StopFn)();

// Per-call latency and throughput data.
// Populated by audioToAudio when outTiming != nullptr.
//
// Because Deepgram streams responses while we are still uploading,
// upload/infer/download can overlap.  We report raw wall-clock timestamps
// so the analysis script can properly characterise the overlap.
struct Timing {
    unsigned long encodeMs       = 0; // PCM→mulaw encode time
    unsigned long uploadMs       = 0; // audioSendDone - audioSendStart
    unsigned long firstResponseMs= 0; // first audio byte received, relative to audioSendStart
    unsigned long lastResponseMs = 0; // AgentAudioDone / collect end, relative to audioSendStart
    unsigned long decodeMs       = 0; // mulaw→PCM16 decode time
    size_t        uploadBytes    = 0; // mulaw bytes sent
    size_t        downloadBytes  = 0; // mulaw bytes received
};

// Throttle uploads to the live mic rate (8 bytes/ms mulaw) so file-based
// trials reproduce live streaming timing. Default off.
extern bool paceRealtime;

// Run Deepgram Voice Agent pipeline:
//   1. Load audioPath WAV from FFAT filesystem
//   2. Encode PCM→µ-law (2× size reduction)
//   3. Connect to Deepgram Voice Agent via WebSocket (TLS)
//   4. Send Settings + wait for SettingsApplied
//   5. Stream µ-law audio as binary WebSocket frames
//   6. Collect spoken response audio frames
//   7. Return audio as a PSRAM-allocated µ-law WAV buffer
//
// On success, *outWav is set to a PSRAM buffer the caller must free(),
// and *outWavLen is set to its byte length. Returns true on success.
//
// outTiming:      optional; filled with per-phase latency data if non-null.
// printBreakdown: when false, the latency summary table is suppressed
//                 (useful when the caller is aggregating many runs).
bool audioToAudio(
    const char* audioPath,
    uint8_t**   outWav,
    size_t*     outWavLen,
    const char* prompt         = "You are a helpful voice assistant. Be concise.",
    const char* voice          = "aura-2-asteria-en",
    Timing*     outTiming      = nullptr,
    bool        printBreakdown = true
);

// Stream live mic audio to Deepgram while recording.
// micReadFn: called repeatedly to get PCM16 samples from the mic.
// stopFn:    called each chunk — return true to stop recording.
// On success, *outPcm16 is a PSRAM buffer of PCM16 samples at 8kHz (caller must free).
// *outPcm16Len is the byte count.
bool liveToAudio(
    MicReadFn   micReadFn,
    StopFn      stopFn,
    uint8_t**   outPcm16,
    size_t*     outPcm16Len,
    const char* prompt = "You are a helpful voice assistant. Be concise.",
    const char* voice  = "aura-2-asteria-en"
);

} // namespace deepgram

#endif
