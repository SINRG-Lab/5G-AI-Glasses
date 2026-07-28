#include "audio_utils.h"
#include "config.h"
#include <FFat.h>
#include <mbedtls/base64.h>

bool audioFsInit()
{
    if (!FFat.begin(false)) {
        Serial.println("FFat mount failed - try 'pio run -t uploadfs'");
        return false;
    }
    Serial.printf("FFat mounted (%u KB free of %u KB)\n",
                  FFat.freeBytes() / 1024, FFat.totalBytes() / 1024);

    // List files for debugging
    File root = FFat.open("/");
    File f = root.openNextFile();
    bool anyFiles = false;
    while (f) {
        Serial.printf("  /%-30s %7u bytes\n", f.name(), f.size());
        anyFiles = true;
        f = root.openNextFile();
    }
    if (!anyFiles) Serial.println("  (filesystem is empty)");

    return true;
}

bool loadAudioFile(const char* path, uint8_t** outBuf, size_t* outLen)
{
    File f = FFat.open(path, "r");
    if (!f) {
        Serial.printf("Cannot open file: %s\n", path);
        return false;
    }

    size_t fileSize = f.size();
    Serial.printf("Loading %s (%u bytes)\n", path, (unsigned)fileSize);

    uint8_t* buf = (uint8_t*)ps_malloc(fileSize);
    if (!buf) {
        Serial.println("PSRAM alloc failed for audio file");
        f.close();
        return false;
    }

    size_t bytesRead = f.read(buf, fileSize);
    f.close();

    if (bytesRead != fileSize) {
        Serial.printf("Read error: got %u of %u bytes\n", (unsigned)bytesRead, (unsigned)fileSize);
        free(buf);
        return false;
    }

    *outBuf = buf;
    *outLen = fileSize;
    return true;
}

const char* detectMimeType(const uint8_t* buf, size_t len)
{
    if (len >= 12 && memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WAVE", 4) == 0) {
        return "audio/wav";
    }
    if (len >= 3 && memcmp(buf, "ID3", 3) == 0) {
        return "audio/mpeg";
    }
    if (len >= 2 && buf[0] == 0xFF && (buf[1] & 0xE0) == 0xE0) {
        return "audio/mpeg";
    }
    Serial.println("Warning: unknown audio format, defaulting to audio/wav");
    return "audio/wav";
}

char* base64Encode(const uint8_t* src, size_t srcLen, size_t* outLen)
{
    // Calculate required output size
    size_t b64Len = 0;
    mbedtls_base64_encode(NULL, 0, &b64Len, src, srcLen);

    char* b64Buf = (char*)ps_malloc(b64Len + 1);
    if (!b64Buf) {
        Serial.println("PSRAM alloc failed for base64 encode");
        return nullptr;
    }

    size_t written = 0;
    int ret = mbedtls_base64_encode((unsigned char*)b64Buf, b64Len + 1, &written, src, srcLen);
    if (ret != 0) {
        Serial.printf("base64 encode failed: %d\n", ret);
        free(b64Buf);
        return nullptr;
    }

    b64Buf[written] = '\0';
    if (outLen) *outLen = written;

#if VERBOSITY >= 1
    Serial.printf("Base64 encoded: %u -> %u chars\n", (unsigned)srcLen, (unsigned)written);
#endif
    return b64Buf;
}

uint8_t* base64Decode(const char* src, size_t srcLen, size_t* outLen)
{
    // Upper bound for decoded size
    size_t maxDecoded = (srcLen / 4) * 3 + 3;

    uint8_t* buf = (uint8_t*)ps_malloc(maxDecoded);
    if (!buf) {
        Serial.println("PSRAM alloc failed for base64 decode");
        return nullptr;
    }

    size_t written = 0;
    int ret = mbedtls_base64_decode(buf, maxDecoded, &written,
                                    (const unsigned char*)src, srcLen);
    if (ret != 0) {
        Serial.printf("base64 decode failed: %d\n", ret);
        free(buf);
        return nullptr;
    }

    if (outLen) *outLen = written;

#if VERBOSITY >= 1
    Serial.printf("Base64 decoded: %u chars -> %u bytes\n", (unsigned)srcLen, (unsigned)written);
#endif
    return buf;
}

static uint8_t encodeLinearToMulaw(int16_t sample)
{
    const int BIAS = 0x84;
    const int CLIP = 32767;

    int sign = 0;
    if (sample < 0) {
        sign   = 0x80;
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

uint8_t* pcm16ToMulaw(const int16_t* pcm, size_t sampleCount, size_t* outLen)
{
    uint8_t* buf = (uint8_t*)ps_malloc(sampleCount);
    if (!buf) {
        Serial.println("PSRAM alloc failed for mulaw encode");
        return nullptr;
    }

    for (size_t i = 0; i < sampleCount; i++)
        buf[i] = encodeLinearToMulaw(pcm[i]);

    if (outLen) *outLen = sampleCount;

#if VERBOSITY >= 1
    Serial.printf("mulaw encode: %u samples -> %u bytes\n",
                  (unsigned)sampleCount, (unsigned)sampleCount);
#endif
    return buf;
}

uint8_t* mulawToPcm16(const uint8_t* mulaw, size_t mulawLen, size_t* outLen)
{
    size_t pcmBytes = mulawLen * 2; // 1 µ-law byte → 1 int16_t sample
    uint8_t* buf = (uint8_t*)ps_malloc(pcmBytes);
    if (!buf) {
        Serial.println("PSRAM alloc failed for mulaw decode");
        return nullptr;
    }

    int16_t* samples = (int16_t*)buf;
    for (size_t i = 0; i < mulawLen; i++) {
        // Standard ITU-T G.711 µ-law expansion
        uint8_t u        = ~mulaw[i];
        int     sign     = (u & 0x80) ? -1 : 1;
        int     exponent = (u >> 4) & 0x07;
        int     mantissa = u & 0x0F;
        int     magnitude = ((mantissa << 3) + 0x84) << exponent;
        samples[i] = (int16_t)(sign * (magnitude - 0x84));
    }

    if (outLen) *outLen = pcmBytes;

#if VERBOSITY >= 1
    Serial.printf("mulaw decode: %u bytes -> %u bytes PCM16\n",
                  (unsigned)mulawLen, (unsigned)pcmBytes);
#endif
    return buf;
}

uint8_t* buildWavFile(const uint8_t* data, size_t dataLen,
                      uint32_t sampleRate, uint16_t bitsPerSample,
                      uint16_t audioFormat, size_t* outLen)
{
    const uint16_t channels   = 1;
    const uint16_t blockAlign = channels * (bitsPerSample / 8);
    const uint32_t byteRate   = sampleRate * blockAlign;
    const uint32_t dataSize   = (uint32_t)dataLen;
    const uint32_t fileSize   = 36 + dataSize;

    size_t totalSize = 44 + dataLen;
    uint8_t* wav = (uint8_t*)ps_malloc(totalSize);
    if (!wav) {
        Serial.println("PSRAM alloc failed for WAV build");
        return nullptr;
    }

    // RIFF header
    memcpy(wav +  0, "RIFF", 4);
    memcpy(wav +  4, &fileSize,   4);
    memcpy(wav +  8, "WAVE", 4);

    // fmt subchunk
    memcpy(wav + 12, "fmt ", 4);
    uint32_t subchunk1Size = 16;
    memcpy(wav + 16, &subchunk1Size, 4);
    memcpy(wav + 20, &audioFormat,   2);
    memcpy(wav + 22, &channels,      2);
    memcpy(wav + 24, &sampleRate,    4);
    memcpy(wav + 28, &byteRate,      4);
    memcpy(wav + 32, &blockAlign,    2);
    memcpy(wav + 34, &bitsPerSample, 2);

    // data subchunk
    memcpy(wav + 36, "data",     4);
    memcpy(wav + 40, &dataSize,  4);
    memcpy(wav + 44, data, dataLen);

    if (outLen) *outLen = totalSize;

#if VERBOSITY >= 1
    Serial.printf("Built WAV: %u bytes (%u Hz, %u-bit, fmt %u)\n",
                  (unsigned)totalSize, (unsigned)sampleRate,
                  (unsigned)bitsPerSample, (unsigned)audioFormat);
#endif
    return wav;
}

uint8_t* buildWavFromPcm(const uint8_t* pcmData, size_t pcmLen, size_t* outLen)
{
    return buildWavFile(pcmData, pcmLen, 24000, 16, 1 /*PCM*/, outLen);
}

bool saveWavFile(const char* path, const uint8_t* data, size_t len)
{
    File f = FFat.open(path, "w");
    if (!f) {
        Serial.printf("Cannot open %s for writing\n", path);
        return false;
    }

    size_t written = f.write(data, len);
    f.close();

    if (written != len) {
        Serial.printf("Write error: wrote %u of %u bytes\n", (unsigned)written, (unsigned)len);
        return false;
    }

    Serial.printf("Saved %s (%u bytes)\n", path, (unsigned)len);
    return true;
}
