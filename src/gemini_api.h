#ifndef GEMINI_API_H
#define GEMINI_API_H

#include <Arduino.h>

namespace gemini {

// Send audio file to Gemini for inference/transcription.
// Returns the response text, or empty string on failure.
String audioToText(const char* audioPath, const char* prompt);

// Send text to Gemini TTS.
// On success, *outWav is set to a PSRAM-allocated WAV buffer the caller must
// free(), and *outWavLen is set to its byte length. Returns true on success.
bool textToAudio(const char* text, uint8_t** outWav, size_t* outWavLen);

} // namespace gemini

#endif
