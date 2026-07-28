#ifndef AUDIO_UTILS_H
#define AUDIO_UTILS_H

#include <Arduino.h>

// Initialize the FFAT filesystem
bool audioFsInit();

// Load an audio file from FFAT into a PSRAM buffer.
// Sets *outBuf and *outLen. Caller must free(*outBuf).
// Returns true on success.
bool loadAudioFile(const char* path, uint8_t** outBuf, size_t* outLen);

// Detect MIME type from file content ("audio/wav" or "audio/mpeg")
const char* detectMimeType(const uint8_t* buf, size_t len);

// Base64 encode using mbedtls. Returns PSRAM-allocated null-terminated string.
// Caller must free() the result.
char* base64Encode(const uint8_t* src, size_t srcLen, size_t* outLen);

// Base64 decode using mbedtls. Returns PSRAM-allocated buffer.
// Caller must free() the result.
uint8_t* base64Decode(const char* src, size_t srcLen, size_t* outLen);

// Decode µ-law (G.711) encoded bytes to signed 16-bit linear PCM.
// Returns PSRAM-allocated buffer of int16_t samples. Caller must free().
// *outLen is set to the output byte count (= 2 × mulawLen).
uint8_t* mulawToPcm16(const uint8_t* mulaw, size_t mulawLen, size_t* outLen);

// Encode signed 16-bit linear PCM samples to µ-law (G.711).
// Returns PSRAM-allocated buffer of mulaw bytes. Caller must free().
// *outLen is set to the output byte count (= sampleCount).
uint8_t* pcm16ToMulaw(const int16_t* pcm, size_t sampleCount, size_t* outLen);

// Build a WAV file for arbitrary format.
// audioFormat: 1=PCM (linear16), 6=A-law, 7=µ-law
// Returns PSRAM-allocated buffer. Caller must free().
uint8_t* buildWavFile(const uint8_t* data, size_t dataLen,
                      uint32_t sampleRate, uint16_t bitsPerSample,
                      uint16_t audioFormat, size_t* outLen);

// Convenience wrapper: 24 kHz 16-bit mono PCM (used by Gemini TTS).
// Returns PSRAM-allocated buffer. Caller must free().
uint8_t* buildWavFromPcm(const uint8_t* pcmData, size_t pcmLen, size_t* outLen);

// Save raw data to a file on FFAT
bool saveWavFile(const char* path, const uint8_t* data, size_t len);

#endif
