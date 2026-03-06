#pragma once

#include <Arduino.h>

class BaiduASRClient {
public:
    struct Config {
        const char* authKey = nullptr;
        const char* apiKey = nullptr;
        const char* secretKey = nullptr;
        const char* oauthHost = "aip.baidubce.com";
        int oauthPort = 443;
        const char* oauthPath = "/oauth/2.0/token";
        const char* asrHost = "vop.baidu.com";
        int asrPort = 80;
        const char* asrPath = "/server_api";
        bool asrUseTls = false;
        const char* cuid = nullptr;
        uint32_t httpTimeoutMs = 15000;
        uint32_t sampleRate = 16000;
        uint16_t devPid = 1537;
    };

    explicit BaiduASRClient(const Config& config);

    void updateConfig(const Config& config);

    bool transcribePCM(const uint8_t* pcmData, size_t pcmBytes, String& outText);

private:
    Config cfg_;
    String cachedAccessToken_;
    uint32_t accessTokenExpiryMs_;

    bool hasValidCredentials() const;
    bool getAccessToken(String& outToken);
    static bool readHttpResponse(Client& client,
                                 uint32_t timeoutMs,
                                 String& outStatus,
                                 String& outBody);
    static String decodeChunkedBody(const String& encodedBody);
    static String urlEncode(const String& value);
};