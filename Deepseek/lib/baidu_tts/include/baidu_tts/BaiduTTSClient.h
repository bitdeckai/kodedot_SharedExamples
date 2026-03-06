#pragma once

#include <Arduino.h>

class BaiduTTSClient {
public:
    struct Config {
        const char* authKey = nullptr;
        const char* apiKey = nullptr;
        const char* secretKey = nullptr;
        const char* oauthHost = "aip.baidubce.com";
        int oauthPort = 443;
        const char* oauthPath = "/oauth/2.0/token";
        const char* ttsHost = "tsn.baidu.com";
        int ttsPort = 443;
        const char* ttsPath = "/text2audio";
        const char* cuid = nullptr;
        const char* language = "zh";
        uint32_t httpTimeoutMs = 15000;
        uint32_t maxAudioBytes = 512 * 1024;
        uint32_t sampleRate = 16000;
        uint8_t speed = 5;
        uint8_t pitch = 5;
        uint8_t volume = 9;
        uint8_t speaker = 0;
        uint8_t audioEncoding = 4;
    };

    explicit BaiduTTSClient(const Config& config);

    void updateConfig(const Config& config);
    bool synthesizePCM(const String& text, uint8_t*& outAudio, size_t& outBytes);

private:
    Config cfg_;
    String cachedAccessToken_;
    uint32_t accessTokenExpiryMs_;

    bool hasValidCredentials() const;
    bool getAccessToken(String& outToken);

    static String urlEncode(const String& value);
    static bool readLine(Client& client, String& outLine, uint32_t timeoutMs);
    static bool readTextHttpResponse(Client& client,
                                     uint32_t timeoutMs,
                                     String& outStatus,
                                     String& outBody);
    static String decodeChunkedBody(const String& encodedBody);
    static bool appendBytes(uint8_t*& buffer,
                            size_t& currentSize,
                            size_t& capacity,
                            const uint8_t* data,
                            size_t bytesToAppend,
                            size_t maxBytes);
    static bool readExactBytes(Client& client,
                               uint8_t* dest,
                               size_t bytesToRead,
                               uint32_t timeoutMs);
    static bool readBinaryBody(Client& client,
                               uint32_t timeoutMs,
                               bool chunked,
                               int contentLength,
                               size_t maxBytes,
                               uint8_t*& outData,
                               size_t& outBytes);
};