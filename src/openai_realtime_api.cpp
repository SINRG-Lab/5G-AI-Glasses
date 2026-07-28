#include "openai_realtime_api.h"
#include "audio_utils.h"
#include "config.h"
#include "modem_setup.h"

#include <ArduinoJson.h>
#include <WalterModem.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <string.h>

namespace openai_realtime {

static const char*     OPENAI_HOST = "api.openai.com";
static const uint16_t  OPENAI_PORT = 443;
static const uint32_t  PCMU_RATE   = 8000;
static const uint16_t  PCMU_FORMAT = 7;
static const uint16_t  PCMU_BITS   = 8;
static const size_t    AUDIO_APPEND_CHUNK = 960;

static size_t wsParsedOffset = 0;

struct CollectState {
    uint8_t*      audioBuf      = nullptr;
    size_t        audioCapacity = 0;
    size_t        audioLen      = 0;
    unsigned long firstAudioMs  = 0;
    unsigned long audioDoneMs   = 0;
    unsigned long inputTokens   = 0;
    unsigned long outputTokens  = 0;
    bool          responseDone  = false;
    bool          failed        = false;
};

struct WavPcm {
    uint8_t* data       = nullptr;
    size_t   len        = 0;
    uint32_t sampleRate = 16000;
    uint16_t channels   = 1;
    uint16_t bits       = 16;
    uint16_t format     = 1;
};

// ================================================================
//  WAV INPUT + 8 KHZ PCMU CONVERSION
// ================================================================

static bool loadWavPcm16Mono(const char* path, WavPcm& out)
{
    uint8_t* fileData = nullptr;
    size_t fileLen = 0;
    if (!loadAudioFile(path, &fileData, &fileLen)) return false;

    if (fileLen < 12 || memcmp(fileData, "RIFF", 4) != 0 ||
        memcmp(fileData + 8, "WAVE", 4) != 0) {
        Serial.println("OpenAI Realtime input must be a WAV file");
        free(fileData);
        return false;
    }

    const uint8_t* pcm = nullptr;
    size_t pcmLen = 0;
    size_t pos = 12;
    bool foundFmt = false;

    while (pos + 8 <= fileLen) {
        uint32_t chunkLen = 0;
        memcpy(&chunkLen, fileData + pos + 4, sizeof(chunkLen));
        size_t dataPos = pos + 8;
        size_t safeLen = (dataPos <= fileLen && chunkLen <= fileLen - dataPos)
                         ? chunkLen : fileLen - dataPos;

        if (memcmp(fileData + pos, "fmt ", 4) == 0 && safeLen >= 16) {
            memcpy(&out.format,     fileData + dataPos + 0,  2);
            memcpy(&out.channels,   fileData + dataPos + 2,  2);
            memcpy(&out.sampleRate, fileData + dataPos + 4,  4);
            memcpy(&out.bits,       fileData + dataPos + 14, 2);
            foundFmt = true;
        } else if (memcmp(fileData + pos, "data", 4) == 0) {
            pcm = fileData + dataPos;
            pcmLen = safeLen;
        }

        if (chunkLen > SIZE_MAX - 9) break;
        pos = dataPos + chunkLen + (chunkLen & 1U);
    }

    if (!foundFmt || !pcm || out.format != 1 || out.channels != 1 || out.bits != 16) {
        Serial.printf("Unsupported WAV: format=%u channels=%u bits=%u\n",
                      out.format, out.channels, out.bits);
        free(fileData);
        return false;
    }

    out.data = (uint8_t*)ps_malloc(pcmLen);
    if (!out.data) {
        Serial.println("PSRAM alloc failed for WAV PCM");
        free(fileData);
        return false;
    }
    memcpy(out.data, pcm, pcmLen);
    out.len = pcmLen;
    free(fileData);
    return true;
}

static uint8_t encodeMulaw(int16_t sample)
{
    const int BIAS = 0x84;
    const int CLIP = 32767;
    int sign = 0;
    if (sample < 0) {
        sign = 0x80;
        sample = (sample == INT16_MIN) ? INT16_MAX : -sample;
    }
    if (sample > CLIP) sample = CLIP;
    sample += BIAS;

    int exponent;
    if      (sample < 0x0100) exponent = 0;
    else if (sample < 0x0200) exponent = 1;
    else if (sample < 0x0400) exponent = 2;
    else if (sample < 0x0800) exponent = 3;
    else if (sample < 0x1000) exponent = 4;
    else if (sample < 0x2000) exponent = 5;
    else if (sample < 0x4000) exponent = 6;
    else                      exponent = 7;

    int mantissa = (sample >> (exponent + 3)) & 0x0F;
    return (uint8_t)(~(sign | (exponent << 4) | mantissa));
}

// Downsample mono PCM16 to the fixed 8 kHz rate required by audio/pcmu.
// Averaging each source window avoids the worst aliasing of simple decimation.
static uint8_t* pcm16ToPcmu8k(const int16_t* pcm, size_t sampleCount,
                              uint32_t inputRate, size_t* outLen)
{
    if (!pcm || sampleCount == 0 || inputRate == 0) return nullptr;
    size_t outputSamples = (size_t)(((uint64_t)sampleCount * PCMU_RATE) / inputRate);
    if (outputSamples == 0) return nullptr;

    uint8_t* out = (uint8_t*)ps_malloc(outputSamples);
    if (!out) {
        Serial.println("PSRAM alloc failed for PCMU input");
        return nullptr;
    }

    for (size_t i = 0; i < outputSamples; i++) {
        size_t begin = (size_t)(((uint64_t)i * inputRate) / PCMU_RATE);
        size_t end = (size_t)(((uint64_t)(i + 1) * inputRate) / PCMU_RATE);
        if (end <= begin) end = begin + 1;
        if (begin >= sampleCount) begin = sampleCount - 1;
        if (end > sampleCount) end = sampleCount;

        int64_t sum = 0;
        for (size_t j = begin; j < end; j++) sum += pcm[j];
        int16_t averaged = (int16_t)(sum / (int64_t)(end - begin));
        out[i] = encodeMulaw(averaged);
    }

    *outLen = outputSamples;
    return out;
}

// ================================================================
//  WEBSOCKET TRANSPORT
// ================================================================

static size_t buildWsFrame(uint8_t* out, uint8_t opcode,
                           const uint8_t* payload, size_t payloadLen)
{
    size_t pos = 0;
    out[pos++] = 0x80 | (opcode & 0x0F);

    if (payloadLen < 126) {
        out[pos++] = 0x80 | (uint8_t)payloadLen;
    } else if (payloadLen < 65536) {
        out[pos++] = 0x80 | 126;
        out[pos++] = (payloadLen >> 8) & 0xFF;
        out[pos++] = payloadLen & 0xFF;
    } else {
        out[pos++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--)
            out[pos++] = (payloadLen >> (i * 8)) & 0xFF;
    }

    uint32_t maskValue = esp_random();
    uint8_t mask[4];
    memcpy(mask, &maskValue, sizeof(mask));
    memcpy(out + pos, mask, sizeof(mask));
    pos += sizeof(mask);

    for (size_t i = 0; i < payloadLen; i++)
        out[pos++] = payload[i] ^ mask[i & 3];
    return pos;
}

static bool wsSocketSend(int socketId, const uint8_t* data, size_t len)
{
    for (size_t offset = 0; offset < len; offset += 1500) {
        size_t chunk = min((size_t)1500, len - offset);
        if (!WalterModem::socketSend(socketId, (uint8_t*)(data + offset),
                                     (uint16_t)chunk)) {
            Serial.printf("OpenAI WS send failed at byte %u\n", (unsigned)offset);
            return false;
        }
        if (offset + chunk < len) delay(5);
    }
    return true;
}

static bool wsSendText(int socketId, const char* text)
{
    size_t textLen = strlen(text);
    uint8_t* frame = (uint8_t*)ps_malloc(textLen + 14);
    if (!frame) return false;
    size_t frameLen = buildWsFrame(frame, 0x1, (const uint8_t*)text, textLen);
    bool ok = wsSocketSend(socketId, frame, frameLen);
    free(frame);
    return ok;
}

static bool wsSendControl(int socketId, uint8_t opcode,
                          const uint8_t* payload = nullptr, size_t payloadLen = 0)
{
    if (payloadLen > 125) return false;
    uint8_t frame[131];
    size_t frameLen = buildWsFrame(frame, opcode, payload, payloadLen);
    return WalterModem::socketSend(socketId, frame, (uint16_t)frameLen);
}

static bool parseNextWsFrame(uint8_t* outOpcode,
                             const uint8_t** outPayload,
                             size_t* outPayloadLen)
{
    size_t availableLen = responseLen;
    if (!responseBuffer || wsParsedOffset > availableLen) return false;
    const uint8_t* buf = responseBuffer + wsParsedOffset;
    size_t available = availableLen - wsParsedOffset;
    if (available < 2) return false;

    uint8_t b0 = buf[0];
    uint8_t b1 = buf[1];
    uint8_t opcode = b0 & 0x0F;
    bool masked = (b1 & 0x80) != 0;
    size_t payloadLen = b1 & 0x7F;
    size_t pos = 2;

    if (payloadLen == 126) {
        if (available < pos + 2) return false;
        payloadLen = ((size_t)buf[pos] << 8) | buf[pos + 1];
        pos += 2;
    } else if (payloadLen == 127) {
        if (available < pos + 8) return false;
        payloadLen = 0;
        for (int i = 0; i < 8; i++) payloadLen = (payloadLen << 8) | buf[pos + i];
        pos += 8;
    }

    uint8_t mask[4] = {};
    if (masked) {
        if (available < pos + 4) return false;
        memcpy(mask, buf + pos, sizeof(mask));
        pos += sizeof(mask);
    }
    if (payloadLen > available - pos) return false;

    if (masked) {
        uint8_t* mutablePayload = responseBuffer + wsParsedOffset + pos;
        for (size_t i = 0; i < payloadLen; i++) mutablePayload[i] ^= mask[i & 3];
    }

    *outOpcode = opcode;
    *outPayload = responseBuffer + wsParsedOffset + pos;
    *outPayloadLen = payloadLen;
    wsParsedOffset += pos + payloadLen;
    return true;
}

static bool wsHandshake(int socketId)
{
    uint8_t keyBytes[16];
    for (int i = 0; i < 4; i++) {
        uint32_t value = esp_random();
        memcpy(keyBytes + i * 4, &value, 4);
    }
    size_t keyLen = 0;
    char* key = base64Encode(keyBytes, sizeof(keyBytes), &keyLen);
    if (!key) return false;

    char path[192];
    snprintf(path, sizeof(path), "/v1/realtime?model=%s", OPENAI_REALTIME_MODEL);

    size_t requestCapacity = 512 + strlen(OPENAI_API_KEY) + strlen(path);
    char* request = (char*)ps_malloc(requestCapacity);
    if (!request) {
        free(key);
        return false;
    }
    int requestLen = snprintf(request, requestCapacity,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Authorization: Bearer %s\r\n"
        "\r\n",
        path, OPENAI_HOST, key, OPENAI_API_KEY);
    free(key);

    bool sent = requestLen > 0 && (size_t)requestLen < requestCapacity &&
                wsSocketSend(socketId, (const uint8_t*)request, requestLen);
    free(request);
    if (!sent) return false;

    unsigned long deadline = millis() + 20000;
    while (millis() < deadline) {
        if (pendingRingBytes > 0 && lastRingMs > 0 &&
            millis() - lastRingMs > RING_CHUNK_TIMEOUT_MS) {
            flushPendingRing();
        }

        size_t availableLen = responseLen;
        if (availableLen > 4) {
            void* separator = memmem(responseBuffer, availableLen, "\r\n\r\n", 4);
            if (separator) {
                size_t headerLen = (uint8_t*)separator - responseBuffer;
                if (memmem(responseBuffer, headerLen, " 101 ", 5)) {
                    wsParsedOffset = headerLen + 4;
                    Serial.println("OpenAI WebSocket handshake OK");
                    return true;
                }
                Serial.printf("OpenAI WS upgrade rejected: %.160s\n",
                              (const char*)responseBuffer);
                return false;
            }
        }
        if (socketDisconnected) break;
        delay(50);
    }
    Serial.println("OpenAI WebSocket handshake timeout");
    return false;
}

static bool waitForEvent(int socketId, const char* wantedType,
                         unsigned long timeoutMs)
{
    unsigned long deadline = millis() + timeoutMs;
    while (millis() < deadline) {
        if (pendingRingBytes > 0 && lastRingMs > 0 &&
            millis() - lastRingMs > RING_CHUNK_TIMEOUT_MS) {
            flushPendingRing();
        }

        uint8_t opcode = 0;
        const uint8_t* payload = nullptr;
        size_t payloadLen = 0;
        while (parseNextWsFrame(&opcode, &payload, &payloadLen)) {
            if (opcode == 0x9) {
                wsSendControl(socketId, 0xA, payload, payloadLen);
                continue;
            }
            if (opcode == 0x8) return false;
            if (opcode != 0x1 || payloadLen == 0) continue;

            JsonDocument doc;
            if (deserializeJson(doc, (const char*)payload, payloadLen)) continue;
            const char* type = doc["type"] | "";
            #if VERBOSITY >= 2
            Serial.printf("  OpenAI event: %s\n", type);
            #endif
            if (strcmp(type, wantedType) == 0) return true;
            if (strcmp(type, "error") == 0) {
                Serial.printf("OpenAI error: %s\n", doc["error"]["message"] | "unknown");
                return false;
            }
        }

        if (socketDisconnected) return false;
        delay(30);
    }
    Serial.printf("Timeout waiting for OpenAI event: %s\n", wantedType);
    return false;
}

static char* buildSessionUpdate(const char* prompt)
{
    JsonDocument doc;
    doc["type"] = "session.update";
    JsonObject session = doc["session"].to<JsonObject>();
    session["type"] = "realtime";
    session["model"] = OPENAI_REALTIME_MODEL;
    session["instructions"] = prompt;
    session["output_modalities"].to<JsonArray>().add("audio");

    JsonObject input = session["audio"]["input"].to<JsonObject>();
    input["format"]["type"] = "audio/pcmu";
    input["turn_detection"] = nullptr;

    JsonObject output = session["audio"]["output"].to<JsonObject>();
    output["format"]["type"] = "audio/pcmu";
    output["voice"] = OPENAI_REALTIME_VOICE;

    session["reasoning"]["effort"] = OPENAI_REALTIME_REASONING_EFFORT;

    size_t len = measureJson(doc);
    char* json = (char*)ps_malloc(len + 1);
    if (!json) return nullptr;
    serializeJson(doc, json, len + 1);
    return json;
}

static bool beginSession(const char* prompt, Timing* timing)
{
    if (strncmp(OPENAI_API_KEY, "YOUR_", 5) == 0 || strlen(OPENAI_API_KEY) < 20) {
        Serial.println("Set OPENAI_API_KEY in src/config.h before using Realtime");
        return false;
    }

    wsParsedOffset = 0;
    resetResponseBuffer(1024 * 1024);

    unsigned long connectStart = millis();
    if (!connectSocket(1, OPENAI_HOST, OPENAI_PORT) || !wsHandshake(1)) {
        closeSocket(1);
        freeResponseBuffer();
        return false;
    }
    if (timing) timing->connectMs = millis() - connectStart;

    unsigned long setupStart = millis();
    if (!waitForEvent(1, "session.created", 20000)) {
        closeSocket(1);
        freeResponseBuffer();
        return false;
    }

    char* update = buildSessionUpdate(prompt);
    if (!update || !wsSendText(1, update)) {
        free(update);
        closeSocket(1);
        freeResponseBuffer();
        return false;
    }
    free(update);

    if (!waitForEvent(1, "session.updated", 20000)) {
        closeSocket(1);
        freeResponseBuffer();
        return false;
    }
    if (timing) timing->sessionSetupMs = millis() - setupStart;

    Serial.printf("OpenAI session ready: model=%s effort=%s voice=%s\n",
                  OPENAI_REALTIME_MODEL, OPENAI_REALTIME_REASONING_EFFORT,
                  OPENAI_REALTIME_VOICE);
    return true;
}

static void endSession()
{
    wsSendControl(1, 0x8);
    delay(100);
    closeSocket(1);
    freeResponseBuffer();
}

// Sends one input_audio_buffer.append event. Keeping the raw chunk at 960
// bytes ensures the Base64 JSON and WebSocket frame fit below the modem's
// 1500-byte transfer ceiling.
static bool sendAudioAppend(int socketId, const uint8_t* audio, size_t audioLen,
                            size_t* wireBytes)
{
    if (audioLen == 0 || audioLen > AUDIO_APPEND_CHUNK) return false;
    static char b64[4 * ((AUDIO_APPEND_CHUNK + 2) / 3) + 1];
    static char json[1400];
    static uint8_t frame[1450];

    size_t b64Len = 0;
    // mbedtls needs the buffer to fit the output PLUS its null terminator;
    // b64[] already reserves that byte, so pass the full size (passing
    // sizeof-1 makes a full 960-byte chunk fail with BUFFER_TOO_SMALL).
    int rc = mbedtls_base64_encode((unsigned char*)b64, sizeof(b64),
                                   &b64Len, audio, audioLen);
    if (rc != 0) return false;
    b64[b64Len] = '\0';

    int jsonLen = snprintf(json, sizeof(json),
        "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}", b64);
    if (jsonLen <= 0 || (size_t)jsonLen >= sizeof(json)) return false;

    size_t frameLen = buildWsFrame(frame, 0x1, (const uint8_t*)json, jsonLen);
    if (frameLen > 1500) return false;
    if (!WalterModem::socketSend(socketId, frame, (uint16_t)frameLen)) return false;
    if (wireBytes) *wireBytes += frameLen;
    return true;
}

static bool decodeAudioDelta(JsonDocument& doc, CollectState& state)
{
    const char* delta = doc["delta"] | "";
    size_t deltaLen = strlen(delta);
    if (deltaLen == 0) return true;
    if (state.audioLen >= state.audioCapacity) return false;

    size_t decodedLen = 0;
    int rc = mbedtls_base64_decode(state.audioBuf + state.audioLen,
                                   state.audioCapacity - state.audioLen,
                                   &decodedLen,
                                   (const unsigned char*)delta, deltaLen);
    if (rc != 0) {
        Serial.printf("OpenAI audio Base64 decode failed: %d\n", rc);
        return false;
    }
    if (decodedLen > 0 && state.firstAudioMs == 0) state.firstAudioMs = millis();
    state.audioLen += decodedLen;
    return true;
}

static bool processAvailableFrames(int socketId, CollectState& state)
{
    bool processed = false;
    uint8_t opcode = 0;
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;

    while (parseNextWsFrame(&opcode, &payload, &payloadLen)) {
        processed = true;
        if (opcode == 0x9) {
            wsSendControl(socketId, 0xA, payload, payloadLen);
            continue;
        }
        if (opcode == 0x8) {
            state.responseDone = true;
            if (state.audioLen == 0) state.failed = true;
            continue;
        }
        if (opcode != 0x1 || payloadLen == 0) continue;

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, (const char*)payload, payloadLen);
        if (err) {
            #if VERBOSITY >= 2
            Serial.printf("OpenAI JSON parse error: %s\n", err.c_str());
            #endif
            continue;
        }

        const char* type = doc["type"] | "";
        #if VERBOSITY >= 2
        Serial.printf("  OpenAI event: %s\n", type);
        #endif

        if (strcmp(type, "response.output_audio.delta") == 0 ||
            strcmp(type, "response.audio.delta") == 0) {
            if (!decodeAudioDelta(doc, state)) {
                state.failed = true;
                state.responseDone = true;
            }
        } else if (strcmp(type, "response.output_audio.done") == 0 ||
                   strcmp(type, "response.audio.done") == 0) {
            state.audioDoneMs = millis();
        } else if (strcmp(type, "response.output_audio_transcript.done") == 0 ||
                   strcmp(type, "response.audio_transcript.done") == 0) {
            #if VERBOSITY >= 1
            Serial.printf("  [assistant]: %.180s\n", doc["transcript"] | "");
            #endif
        } else if (strcmp(type, "response.done") == 0) {
            const char* status = doc["response"]["status"] | "completed";
            state.inputTokens = doc["response"]["usage"]["input_tokens"] | 0UL;
            state.outputTokens = doc["response"]["usage"]["output_tokens"] | 0UL;
            if (strcmp(status, "completed") != 0) {
                Serial.printf("OpenAI response status: %s\n", status);
                state.failed = true;
            }
            if (state.audioDoneMs == 0) state.audioDoneMs = millis();
            state.responseDone = true;
        } else if (strcmp(type, "error") == 0) {
            Serial.printf("OpenAI error: %s (%s)\n",
                          doc["error"]["message"] | "unknown",
                          doc["error"]["code"] | "no_code");
            state.failed = true;
            state.responseDone = true;
        }
    }
    return processed;
}

static bool drainIncoming(int socketId, CollectState& state)
{
    if (pendingRingBytes > 0 && lastRingMs > 0 &&
        millis() - lastRingMs > RING_CHUNK_TIMEOUT_MS) {
        flushPendingRing();
    }
    return processAvailableFrames(socketId, state);
}

static void collectResponse(int socketId, CollectState& state)
{
    unsigned long start = millis();
    unsigned long lastData = start;
    unsigned long lastPing = start;
    const unsigned long hardTimeoutMs = 180000;
    const unsigned long idleTimeoutMs = 120000;

    while (!state.responseDone && millis() - start < hardTimeoutMs) {
        bool gotData = drainIncoming(socketId, state);
        if (gotData) lastData = millis();

        if (socketDisconnected) {
            while (pendingRingBytes > 0 && flushPendingRing()) {}
            processAvailableFrames(socketId, state);
            break;
        }
        if (millis() - lastPing >= 20000) {
            wsSendControl(socketId, 0x9);
            lastPing = millis();
        }
        if (millis() - lastData > idleTimeoutMs) {
            Serial.println("OpenAI response timed out with no new data");
            break;
        }
        delay(30);
    }
}

static bool finishInputAndRequestResponse(int socketId,
                                          unsigned long* responseCreateMs)
{
    if (!wsSendText(socketId, "{\"type\":\"input_audio_buffer.commit\"}"))
        return false;
    *responseCreateMs = millis();
    return wsSendText(socketId, "{\"type\":\"response.create\"}");
}

// ================================================================
//  PUBLIC FILE BENCHMARK API
// ================================================================

bool audioToAudio(const char* audioPath,
                  uint8_t** outWav, size_t* outWavLen,
                  const char* prompt,
                  Timing* outTiming, bool printBreakdown)
{
    if (!outWav || !outWavLen) return false;
    *outWav = nullptr;
    *outWavLen = 0;
    Timing timing;
    unsigned long callStart = millis();

    Serial.println("\n--- OpenAI Realtime Audio-to-Audio ---");
    WavPcm wav;
    if (!loadWavPcm16Mono(audioPath, wav)) return false;
    size_t pcmInputLen = wav.len;

    unsigned long encodeStart = millis();
    size_t pcmuInputLen = 0;
    uint8_t* pcmuInput = pcm16ToPcmu8k((const int16_t*)wav.data,
                                       wav.len / 2, wav.sampleRate,
                                       &pcmuInputLen);
    timing.encodeMs = millis() - encodeStart;
    free(wav.data);
    if (!pcmuInput) return false;

    Serial.printf("Input: %u PCM16 bytes @ %u Hz -> %u PCMU bytes @ 8 kHz\n",
                  (unsigned)pcmInputLen, (unsigned)wav.sampleRate,
                  (unsigned)pcmuInputLen);

    if (!beginSession(prompt, &timing)) {
        free(pcmuInput);
        return false;
    }

    const size_t outputCapacity = 300 * 1024;
    uint8_t* output = (uint8_t*)ps_malloc(outputCapacity);
    if (!output) {
        free(pcmuInput);
        endSession();
        return false;
    }
    CollectState state;
    state.audioBuf = output;
    state.audioCapacity = outputCapacity;

    unsigned long audioSendStart = millis();
    for (size_t offset = 0; offset < pcmuInputLen; offset += AUDIO_APPEND_CHUNK) {
        size_t chunk = min(AUDIO_APPEND_CHUNK, pcmuInputLen - offset);
        if (!sendAudioAppend(1, pcmuInput + offset, chunk, &timing.wireUploadBytes)) {
            Serial.printf("OpenAI audio upload failed at byte %u\n", (unsigned)offset);
            free(pcmuInput);
            free(output);
            endSession();
            return false;
        }
        drainIncoming(1, state);
    }
    timing.uploadMs = millis() - audioSendStart;
    timing.uploadBytes = pcmuInputLen;
    free(pcmuInput);

    unsigned long responseCreateMs = 0;
    if (!finishInputAndRequestResponse(1, &responseCreateMs)) {
        free(output);
        endSession();
        return false;
    }
    collectResponse(1, state);
    unsigned long responseEndMs = state.audioDoneMs ? state.audioDoneMs : millis();

    timing.firstResponseMs = state.firstAudioMs > audioSendStart
                             ? state.firstAudioMs - audioSendStart : 0;
    timing.lastResponseMs = responseEndMs - audioSendStart;
    timing.responseWaitMs = state.firstAudioMs > responseCreateMs
                            ? state.firstAudioMs - responseCreateMs : 0;
    timing.downloadBytes = state.audioLen;
    timing.inputTokens = state.inputTokens;
    timing.outputTokens = state.outputTokens;

    size_t decodedLen = 0;
    unsigned long decodeStart = millis();
    uint8_t* decoded = state.audioLen
                       ? mulawToPcm16(output, state.audioLen, &decodedLen) : nullptr;
    timing.decodeMs = millis() - decodeStart;
    free(decoded);

    timing.totalMs = millis() - callStart;
    endSession();

    if (state.failed || state.audioLen == 0) {
        Serial.println("No usable audio received from OpenAI Realtime");
        free(output);
        if (outTiming) *outTiming = timing;
        return false;
    }

    uint8_t* result = buildWavFile(output, state.audioLen, PCMU_RATE,
                                   PCMU_BITS, PCMU_FORMAT, outWavLen);
    free(output);
    if (!result) return false;
    *outWav = result;

    if (printBreakdown) {
        Serial.println("\n==== OpenAI Realtime Latency Breakdown ====");
        Serial.printf("  Model / effort:          %s / %s\n",
                      OPENAI_REALTIME_MODEL, OPENAI_REALTIME_REASONING_EFFORT);
        Serial.printf("  Encode to 8k PCMU:       %5lu ms\n", timing.encodeMs);
        Serial.printf("  Connect + WS handshake:  %5lu ms\n", timing.connectMs);
        Serial.printf("  Session configuration:   %5lu ms\n", timing.sessionSetupMs);
        Serial.printf("  Upload audio:            %5lu ms (%u raw, %u wire bytes)\n",
                      timing.uploadMs, (unsigned)timing.uploadBytes,
                      (unsigned)timing.wireUploadBytes);
        Serial.printf("  Response-create -> audio:%5lu ms\n", timing.responseWaitMs);
        Serial.printf("  First audio from send:   %5lu ms\n", timing.firstResponseMs);
        Serial.printf("  Final audio from send:   %5lu ms (%u bytes)\n",
                      timing.lastResponseMs, (unsigned)timing.downloadBytes);
        Serial.printf("  Decode PCMU -> PCM16:    %5lu ms\n", timing.decodeMs);
        Serial.printf("  Full call:               %5lu ms\n", timing.totalMs);
        Serial.printf("  Tokens (input/output):   %lu / %lu\n",
                      timing.inputTokens, timing.outputTokens);
        Serial.println("===========================================\n");
    }

    if (outTiming) *outTiming = timing;
    return true;
}

// ================================================================
//  PUBLIC LIVE MICROPHONE API
// ================================================================

bool liveToAudio(MicReadFn micReadFn, StopFn stopFn,
                 uint8_t** outPcm16, size_t* outPcm16Len,
                 const char* prompt)
{
    if (!micReadFn || !stopFn || !outPcm16 || !outPcm16Len) return false;
    *outPcm16 = nullptr;
    *outPcm16Len = 0;

    Serial.println("\n--- OpenAI Realtime Live Agent ---");
    if (!beginSession(prompt, nullptr)) return false;

    const size_t outputCapacity = 300 * 1024;
    uint8_t* output = (uint8_t*)ps_malloc(outputCapacity);
    if (!output) {
        endSession();
        return false;
    }
    CollectState state;
    state.audioBuf = output;
    state.audioCapacity = outputCapacity;

    static int16_t micPcm[512];
    static uint8_t pcmuBatch[AUDIO_APPEND_CHUNK];
    size_t batchLen = 0;
    size_t ignoredWireBytes = 0;
    unsigned long recordStart = millis();
    unsigned long lastStopCheck = recordStart;

    Serial.println("Recording - press button to stop.");
    while (true) {
        size_t samples = micReadFn(micPcm, 512);
        for (size_t i = 0; i + 1 < samples; i += 2) {
            int16_t averaged = (int16_t)(((int32_t)micPcm[i] + micPcm[i + 1]) / 2);
            pcmuBatch[batchLen++] = encodeMulaw(averaged);
            if (batchLen == sizeof(pcmuBatch)) {
                if (!sendAudioAppend(1, pcmuBatch, batchLen, &ignoredWireBytes)) {
                    free(output);
                    endSession();
                    return false;
                }
                batchLen = 0;
                drainIncoming(1, state);
            }
        }

        if (millis() - lastStopCheck >= 100) {
            lastStopCheck = millis();
            if (millis() - recordStart >= 500 && stopFn()) break;
        }
    }

    if (batchLen > 0 && !sendAudioAppend(1, pcmuBatch, batchLen, &ignoredWireBytes)) {
        free(output);
        endSession();
        return false;
    }

    unsigned long responseCreateMs = 0;
    if (!finishInputAndRequestResponse(1, &responseCreateMs)) {
        free(output);
        endSession();
        return false;
    }
    Serial.printf("Recorded %.2f s; waiting for %s (%s effort)\n",
                  (millis() - recordStart) / 1000.0f,
                  OPENAI_REALTIME_MODEL, OPENAI_REALTIME_REASONING_EFFORT);
    collectResponse(1, state);
    endSession();

    if (state.failed || state.audioLen == 0) {
        free(output);
        return false;
    }

    uint8_t* pcm16 = mulawToPcm16(output, state.audioLen, outPcm16Len);
    free(output);
    if (!pcm16) return false;
    *outPcm16 = pcm16;
    return true;
}

} // namespace openai_realtime
