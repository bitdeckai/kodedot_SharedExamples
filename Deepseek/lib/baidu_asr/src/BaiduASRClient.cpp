#include <Arduino.h>
#include <Client.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "baidu_asr/BaiduASRClient.h"

BaiduASRClient::BaiduASRClient(const Config& config)
    : cfg_(config)
    , cachedAccessToken_()
    , accessTokenExpiryMs_(0) {}

void BaiduASRClient::updateConfig(const Config& config) {
    cfg_ = config;
    cachedAccessToken_ = "";
    accessTokenExpiryMs_ = 0;
}

bool BaiduASRClient::hasValidCredentials() const {
    const bool hasAuthKey = cfg_.authKey && cfg_.authKey[0] != '\0';
    const bool hasAkSk = cfg_.apiKey && cfg_.apiKey[0] != '\0' &&
                         cfg_.secretKey && cfg_.secretKey[0] != '\0';
    return hasAuthKey || hasAkSk;
}

String BaiduASRClient::urlEncode(const String& value) {
    static const char HEX_DIGITS[] = "0123456789ABCDEF";
    String encoded;
    encoded.reserve(value.length() * 3);
    for (size_t i = 0; i < value.length(); ++i) {
        const uint8_t c = static_cast<uint8_t>(value[i]);
        const bool safe =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            encoded += static_cast<char>(c);
        } else {
            encoded += '%';
            encoded += HEX_DIGITS[(c >> 4) & 0x0F];
            encoded += HEX_DIGITS[c & 0x0F];
        }
    }
    return encoded;
}

String BaiduASRClient::decodeChunkedBody(const String& encodedBody) {
    String decoded;
    decoded.reserve(encodedBody.length());

    int cursor = 0;
    while (cursor < (int)encodedBody.length()) {
        int lineEnd = encodedBody.indexOf("\r\n", cursor);
        int lineAdvance = 2;
        if (lineEnd < 0) {
            lineEnd = encodedBody.indexOf('\n', cursor);
            lineAdvance = 1;
        }
        if (lineEnd < 0) {
            break;
        }

        String sizeLine = encodedBody.substring(cursor, lineEnd);
        sizeLine.trim();
        if (sizeLine.length() == 0) {
            cursor = lineEnd + lineAdvance;
            continue;
        }

        int semicolon = sizeLine.indexOf(';');
        if (semicolon >= 0) {
            sizeLine = sizeLine.substring(0, semicolon);
            sizeLine.trim();
        }

        char* endPtr = nullptr;
        long chunkSize = strtol(sizeLine.c_str(), &endPtr, 16);
        if (endPtr == sizeLine.c_str()) {
            return encodedBody;
        }

        cursor = lineEnd + lineAdvance;
        if (chunkSize <= 0) {
            break;
        }

        if (cursor + chunkSize > (int)encodedBody.length()) {
            return encodedBody;
        }

        decoded += encodedBody.substring(cursor, cursor + chunkSize);
        cursor += chunkSize;

        if (encodedBody.startsWith("\r\n", cursor)) {
            cursor += 2;
        } else if (encodedBody.startsWith("\n", cursor)) {
            cursor += 1;
        }
    }

    return decoded.length() > 0 ? decoded : encodedBody;
}

bool BaiduASRClient::readHttpResponse(Client& client,
                                      uint32_t timeoutMs,
                                      String& outStatus,
                                      String& outBody) {
    String rawResponse;
    rawResponse.reserve(4096);

    uint32_t idleStart = millis();
    while (client.connected() || client.available()) {
        int bytesAvailable = client.available();
        if (bytesAvailable > 0) {
            char buffer[256];
            size_t toRead = bytesAvailable > (int)(sizeof(buffer) - 1) ? (sizeof(buffer) - 1) : (size_t)bytesAvailable;
            int bytesRead = client.read(reinterpret_cast<uint8_t*>(buffer), toRead);
            if (bytesRead > 0) {
                buffer[bytesRead] = '\0';
                rawResponse += buffer;
                idleStart = millis();
                if (rawResponse.length() > 65536) {
                    break;
                }
            }
        } else if (millis() - idleStart > timeoutMs) {
            Serial.println("[BaiduASR] WARNING: timed out waiting for HTTP response");
            break;
        } else {
            delay(10);
        }
        yield();
    }

    int statusEnd = rawResponse.indexOf("\r\n");
    if (statusEnd < 0) {
        statusEnd = rawResponse.indexOf('\n');
    }
    if (statusEnd < 0) {
        outStatus = rawResponse;
        outBody = "";
        return outStatus.length() > 0;
    }

    outStatus = rawResponse.substring(0, statusEnd);
    int bodyStart = rawResponse.indexOf("\r\n\r\n");
    String headers;
    if (bodyStart >= 0) {
        headers = rawResponse.substring(statusEnd + 2, bodyStart);
        outBody = rawResponse.substring(bodyStart + 4);
    } else {
        bodyStart = rawResponse.indexOf("\n\n");
        headers = bodyStart >= 0 ? rawResponse.substring(statusEnd + 1, bodyStart) : "";
        outBody = bodyStart >= 0 ? rawResponse.substring(bodyStart + 2) : "";
    }

    if (headers.indexOf("Transfer-Encoding: chunked") >= 0 ||
        headers.indexOf("transfer-encoding: chunked") >= 0) {
        outBody = decodeChunkedBody(outBody);
    }

    return outStatus.length() > 0;
}

bool BaiduASRClient::getAccessToken(String& outToken) {
    outToken = "";
    if (cfg_.authKey && cfg_.authKey[0] != '\0') {
        return true;
    }
    if (!cfg_.apiKey || !cfg_.apiKey[0] || !cfg_.secretKey || !cfg_.secretKey[0]) {
        Serial.println("[BaiduASR] ERROR: Missing API Key or Secret Key");
        return false;
    }

    const uint32_t now = millis();
    if (cachedAccessToken_.length() > 0 && now < accessTokenExpiryMs_) {
        outToken = cachedAccessToken_;
        return true;
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(cfg_.httpTimeoutMs);

    Serial.printf("[BaiduASR] Requesting access token from %s:%d\n", cfg_.oauthHost, cfg_.oauthPort);
    if (!client.connect(cfg_.oauthHost, cfg_.oauthPort)) {
        Serial.println("[BaiduASR] ERROR: OAuth TLS connect failed");
        return false;
    }

    String path = String(cfg_.oauthPath ? cfg_.oauthPath : "/oauth/2.0/token") +
                  "?grant_type=client_credentials&client_id=" + urlEncode(String(cfg_.apiKey)) +
                  "&client_secret=" + urlEncode(String(cfg_.secretKey));
    client.print(String("POST ") + path + " HTTP/1.1\r\n");
    client.print(String("Host: ") + cfg_.oauthHost + "\r\n");
    client.print("Content-Type: application/json\r\n");
    client.print("Accept: application/json\r\n");
    client.print("Accept-Encoding: identity\r\n");
    client.print("User-Agent: CuteAssistant/1.0\r\n");
    client.print("Content-Length: 0\r\n");
    client.print("Connection: close\r\n\r\n");

    String status;
    String body;
    if (!readHttpResponse(client, cfg_.httpTimeoutMs, status, body)) {
        Serial.println("[BaiduASR] ERROR: OAuth response was empty");
        return false;
    }
    if (!(status.indexOf(" 200 ") >= 0 || status.startsWith("HTTP/1.1 200") || status.startsWith("HTTP/1.0 200"))) {
        Serial.print("[BaiduASR] OAuth status: ");
        Serial.println(status);
        Serial.println(body);
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("[BaiduASR] ERROR: OAuth JSON parse failed: %s\n", err.c_str());
        Serial.print("[BaiduASR] OAuth status: ");
        Serial.println(status);
        Serial.print("[BaiduASR] OAuth raw body: ");
        Serial.println(body);
        return false;
    }

    const char* token = doc["access_token"] | "";
    const uint32_t expiresInSec = doc["expires_in"] | 0;
    if (!token[0]) {
        const char* errorCode = doc["error"] | "unknown_error";
        const char* errorDesc = doc["error_description"] | "";
        Serial.printf("[BaiduASR] ERROR: OAuth failed: %s %s\n", errorCode, errorDesc);
        return false;
    }

    cachedAccessToken_ = token;
    outToken = cachedAccessToken_;

    uint32_t ttlMs = expiresInSec > 60 ? (expiresInSec - 60) * 1000UL : expiresInSec * 1000UL;
    accessTokenExpiryMs_ = millis() + ttlMs;
    Serial.printf("[BaiduASR] Access token acquired, ttl=%lu s\n", (unsigned long)expiresInSec);
    return true;
}

bool BaiduASRClient::transcribePCM(const uint8_t* pcmData, size_t pcmBytes, String& outText) {
    outText = "";
    if (!pcmData || pcmBytes == 0) {
        Serial.println("[BaiduASR] ERROR: PCM buffer is empty");
        return false;
    }
    if (!hasValidCredentials()) {
        Serial.println("[BaiduASR] ERROR: No valid ASR credentials configured");
        return false;
    }

    String token;
    if (!getAccessToken(token)) {
        return false;
    }

    String cuid = cfg_.cuid && cfg_.cuid[0] ? String(cfg_.cuid) : WiFi.macAddress();
    String path = String(cfg_.asrPath ? cfg_.asrPath : "/server_api") +
                  "?dev_pid=" + String(cfg_.devPid) +
                  "&cuid=" + urlEncode(cuid);
    if (cfg_.authKey && cfg_.authKey[0] != '\0') {
        Serial.println("[BaiduASR] Using Authorization header authentication");
    } else {
        path += "&token=" + urlEncode(token);
    }

    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    Client* client = nullptr;
    if (cfg_.asrUseTls) {
        secureClient.setInsecure();
        secureClient.setTimeout(cfg_.httpTimeoutMs);
        if (!secureClient.connect(cfg_.asrHost, cfg_.asrPort)) {
            Serial.println("[BaiduASR] ERROR: ASR TLS connect failed");
            return false;
        }
        client = &secureClient;
    } else {
        plainClient.setTimeout(cfg_.httpTimeoutMs);
        if (!plainClient.connect(cfg_.asrHost, cfg_.asrPort)) {
            Serial.println("[BaiduASR] ERROR: ASR connect failed");
            return false;
        }
        client = &plainClient;
    }

    Serial.printf("[BaiduASR] Uploading %u bytes PCM to %s:%d%s\n",
                  (unsigned)pcmBytes,
                  cfg_.asrHost,
                  cfg_.asrPort,
                  path.c_str());

    client->print(String("POST ") + path + " HTTP/1.1\r\n");
    client->print(String("Host: ") + cfg_.asrHost + "\r\n");
    client->print(String("Content-Type: audio/pcm;rate=") + String(cfg_.sampleRate) + "\r\n");
    client->print(String("Content-Length: ") + String((unsigned)pcmBytes) + "\r\n");
    client->print("Accept: application/json\r\n");
    client->print("Accept-Encoding: identity\r\n");
    client->print("User-Agent: CuteAssistant/1.0\r\n");
    client->print("Connection: close\r\n");
    if (cfg_.authKey && cfg_.authKey[0] != '\0') {
        client->print(String("Authorization: Bearer ") + cfg_.authKey + "\r\n");
    }
    client->print("\r\n");
    client->write(pcmData, pcmBytes);

    String status;
    String body;
    if (!readHttpResponse(*client, cfg_.httpTimeoutMs, status, body)) {
        Serial.println("[BaiduASR] ERROR: ASR response was empty");
        return false;
    }
    if (!(status.indexOf(" 200 ") >= 0 || status.startsWith("HTTP/1.1 200") || status.startsWith("HTTP/1.0 200"))) {
        Serial.print("[BaiduASR] HTTP status: ");
        Serial.println(status);
        Serial.println(body);
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("[BaiduASR] ERROR: ASR JSON parse failed: %s\n", err.c_str());
        Serial.print("[BaiduASR] ASR status: ");
        Serial.println(status);
        Serial.print("[BaiduASR] ASR raw body: ");
        Serial.println(body);
        return false;
    }

    long errNo = doc["err_no"] | -1;
    if (errNo != 0) {
        const char* errMsg = doc["err_msg"] | "unknown error";
        const char* sn = doc["sn"] | "";
        Serial.printf("[BaiduASR] ERROR: err_no=%ld err_msg=%s sn=%s\n", errNo, errMsg, sn);
        return false;
    }

    JsonArray result = doc["result"].as<JsonArray>();
    if (result.isNull() || result.size() == 0) {
        Serial.println("[BaiduASR] ERROR: No recognition result returned");
        return false;
    }

    const char* text = result[0] | "";
    outText = text;
    outText.trim();
    Serial.printf("[BaiduASR] Transcript: %s\n", outText.c_str());
    return outText.length() > 0;
}