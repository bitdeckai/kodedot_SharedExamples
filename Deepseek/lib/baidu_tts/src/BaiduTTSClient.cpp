#include <Arduino.h>
#include <Client.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <cstdlib>
#include <cstring>
#include "baidu_tts/BaiduTTSClient.h"

BaiduTTSClient::BaiduTTSClient(const Config& config)
    : cfg_(config)
    , cachedAccessToken_()
    , accessTokenExpiryMs_(0) {}

void BaiduTTSClient::updateConfig(const Config& config) {
    cfg_ = config;
    cachedAccessToken_ = "";
    accessTokenExpiryMs_ = 0;
}

bool BaiduTTSClient::hasValidCredentials() const {
    const bool hasAuthKey = cfg_.authKey && cfg_.authKey[0] != '\0';
    const bool hasAkSk = cfg_.apiKey && cfg_.apiKey[0] != '\0' &&
                         cfg_.secretKey && cfg_.secretKey[0] != '\0';
    return hasAuthKey || hasAkSk;
}

String BaiduTTSClient::urlEncode(const String& value) {
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

String BaiduTTSClient::decodeChunkedBody(const String& encodedBody) {
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

bool BaiduTTSClient::readLine(Client& client, String& outLine, uint32_t timeoutMs) {
    outLine = "";
    uint32_t idleStart = millis();

    while (client.connected() || client.available()) {
        while (client.available()) {
            const int value = client.read();
            if (value < 0) {
                break;
            }

            idleStart = millis();
            if (value == '\n') {
                if (outLine.endsWith("\r")) {
                    outLine.remove(outLine.length() - 1);
                }
                return true;
            }

            outLine += static_cast<char>(value);
        }

        if (millis() - idleStart > timeoutMs) {
            break;
        }

        delay(1);
        yield();
    }

    return outLine.length() > 0;
}

bool BaiduTTSClient::readTextHttpResponse(Client& client,
                                          uint32_t timeoutMs,
                                          String& outStatus,
                                          String& outBody) {
    String rawResponse;
    rawResponse.reserve(4096);

    uint32_t idleStart = millis();
    while (client.connected() || client.available()) {
        const int bytesAvailable = client.available();
        if (bytesAvailable > 0) {
            char buffer[256];
            const size_t toRead = bytesAvailable > (int)(sizeof(buffer) - 1) ?
                                  (sizeof(buffer) - 1) :
                                  (size_t)bytesAvailable;
            const int bytesRead = client.read(reinterpret_cast<uint8_t*>(buffer), toRead);
            if (bytesRead > 0) {
                buffer[bytesRead] = '\0';
                rawResponse += buffer;
                idleStart = millis();
                if (rawResponse.length() > 65536) {
                    break;
                }
            }
        } else if (millis() - idleStart > timeoutMs) {
            Serial.println("[BaiduTTS] WARNING: timed out waiting for HTTP response");
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

bool BaiduTTSClient::appendBytes(uint8_t*& buffer,
                                 size_t& currentSize,
                                 size_t& capacity,
                                 const uint8_t* data,
                                 size_t bytesToAppend,
                                 size_t maxBytes) {
    if (currentSize + bytesToAppend > maxBytes) {
        return false;
    }
    if (currentSize + bytesToAppend > capacity) {
        size_t newCapacity = capacity == 0 ? 4096 : capacity;
        while (newCapacity < currentSize + bytesToAppend) {
            newCapacity *= 2;
        }
        if (newCapacity > maxBytes) {
            newCapacity = maxBytes;
        }
        if (newCapacity < currentSize + bytesToAppend) {
            return false;
        }

        uint8_t* resized = static_cast<uint8_t*>(realloc(buffer, newCapacity));
        if (!resized) {
            return false;
        }
        buffer = resized;
        capacity = newCapacity;
    }

    if (data && bytesToAppend > 0) {
        memcpy(buffer + currentSize, data, bytesToAppend);
    }
    currentSize += bytesToAppend;
    return true;
}

bool BaiduTTSClient::readExactBytes(Client& client,
                                    uint8_t* dest,
                                    size_t bytesToRead,
                                    uint32_t timeoutMs) {
    size_t totalRead = 0;
    uint32_t idleStart = millis();

    while (totalRead < bytesToRead && (client.connected() || client.available())) {
        const int bytesAvailable = client.available();
        if (bytesAvailable > 0) {
            const size_t toRead = (size_t)bytesAvailable > (bytesToRead - totalRead) ?
                                  (bytesToRead - totalRead) :
                                  (size_t)bytesAvailable;
            const int bytesRead = client.read(dest + totalRead, toRead);
            if (bytesRead > 0) {
                totalRead += (size_t)bytesRead;
                idleStart = millis();
            }
        } else if (millis() - idleStart > timeoutMs) {
            break;
        } else {
            delay(1);
        }
        yield();
    }

    return totalRead == bytesToRead;
}

bool BaiduTTSClient::readBinaryBody(Client& client,
                                    uint32_t timeoutMs,
                                    bool chunked,
                                    int contentLength,
                                    size_t maxBytes,
                                    uint8_t*& outData,
                                    size_t& outBytes) {
    outData = nullptr;
    outBytes = 0;

    uint8_t* buffer = nullptr;
    size_t capacity = 0;
    size_t size = 0;

    if (chunked) {
        while (true) {
            String chunkLine;
            if (!readLine(client, chunkLine, timeoutMs)) {
                free(buffer);
                return false;
            }
            chunkLine.trim();
            if (chunkLine.length() == 0) {
                continue;
            }

            const int semicolon = chunkLine.indexOf(';');
            if (semicolon >= 0) {
                chunkLine = chunkLine.substring(0, semicolon);
                chunkLine.trim();
            }

            char* endPtr = nullptr;
            const long chunkSize = strtol(chunkLine.c_str(), &endPtr, 16);
            if (endPtr == chunkLine.c_str() || chunkSize < 0) {
                free(buffer);
                return false;
            }
            if (chunkSize == 0) {
                String trailer;
                readLine(client, trailer, timeoutMs);
                break;
            }

            const size_t previousSize = size;
            if (!appendBytes(buffer,
                             size,
                             capacity,
                             nullptr,
                             (size_t)chunkSize,
                             maxBytes)) {
                free(buffer);
                return false;
            }
            if (!readExactBytes(client, buffer + previousSize, (size_t)chunkSize, timeoutMs)) {
                free(buffer);
                return false;
            }

            uint8_t chunkEnding[2];
            if (!readExactBytes(client, chunkEnding, sizeof(chunkEnding), timeoutMs)) {
                free(buffer);
                return false;
            }
        }
    } else if (contentLength >= 0) {
        const size_t bodySize = static_cast<size_t>(contentLength);
        if (bodySize == 0 || bodySize > maxBytes) {
            return false;
        }

        buffer = static_cast<uint8_t*>(malloc(bodySize));
        if (!buffer) {
            return false;
        }
        if (!readExactBytes(client, buffer, bodySize, timeoutMs)) {
            free(buffer);
            return false;
        }
        size = bodySize;
        capacity = bodySize;
    } else {
        uint32_t idleStart = millis();
        while (client.connected() || client.available()) {
            const int bytesAvailable = client.available();
            if (bytesAvailable > 0) {
                uint8_t temp[256];
                const size_t toRead = bytesAvailable > (int)sizeof(temp) ? sizeof(temp) : (size_t)bytesAvailable;
                const int bytesRead = client.read(temp, toRead);
                if (bytesRead > 0) {
                    if (!appendBytes(buffer, size, capacity, temp, (size_t)bytesRead, maxBytes)) {
                        free(buffer);
                        return false;
                    }
                    idleStart = millis();
                }
            } else if (millis() - idleStart > timeoutMs) {
                break;
            } else {
                delay(1);
            }
            yield();
        }
    }

    if (!buffer || size == 0) {
        free(buffer);
        return false;
    }

    outData = buffer;
    outBytes = size;
    return true;
}

bool BaiduTTSClient::getAccessToken(String& outToken) {
    outToken = "";
    if (cfg_.authKey && cfg_.authKey[0] != '\0') {
        outToken = cfg_.authKey;
        return true;
    }
    if (!cfg_.apiKey || !cfg_.apiKey[0] || !cfg_.secretKey || !cfg_.secretKey[0]) {
        Serial.println("[BaiduTTS] ERROR: Missing API Key or Secret Key");
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

    if (!client.connect(cfg_.oauthHost, cfg_.oauthPort)) {
        Serial.println("[BaiduTTS] ERROR: OAuth TLS connect failed");
        return false;
    }

    const String path = String(cfg_.oauthPath ? cfg_.oauthPath : "/oauth/2.0/token") +
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
    if (!readTextHttpResponse(client, cfg_.httpTimeoutMs, status, body)) {
        Serial.println("[BaiduTTS] ERROR: OAuth response was empty");
        return false;
    }
    if (!(status.indexOf(" 200 ") >= 0 || status.startsWith("HTTP/1.1 200") || status.startsWith("HTTP/1.0 200"))) {
        Serial.print("[BaiduTTS] OAuth status: ");
        Serial.println(status);
        Serial.println(body);
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("[BaiduTTS] ERROR: OAuth JSON parse failed: %s\n", err.c_str());
        Serial.print("[BaiduTTS] OAuth raw body: ");
        Serial.println(body);
        return false;
    }

    const char* token = doc["access_token"] | "";
    const uint32_t expiresInSec = doc["expires_in"] | 0;
    if (!token[0]) {
        const char* errorCode = doc["error"] | "unknown_error";
        const char* errorDesc = doc["error_description"] | "";
        Serial.printf("[BaiduTTS] ERROR: OAuth failed: %s %s\n", errorCode, errorDesc);
        return false;
    }

    cachedAccessToken_ = token;
    outToken = cachedAccessToken_;
    const uint32_t ttlMs = expiresInSec > 60 ? (expiresInSec - 60) * 1000UL : expiresInSec * 1000UL;
    accessTokenExpiryMs_ = millis() + ttlMs;
    return true;
}

bool BaiduTTSClient::synthesizePCM(const String& text, uint8_t*& outAudio, size_t& outBytes) {
    outAudio = nullptr;
    outBytes = 0;

    String normalizedText = text;
    normalizedText.trim();
    if (normalizedText.length() == 0) {
        Serial.println("[BaiduTTS] ERROR: Empty text to synthesize");
        return false;
    }
    if (!hasValidCredentials()) {
        Serial.println("[BaiduTTS] ERROR: No valid TTS credentials configured");
        return false;
    }

    String token;
    if (!getAccessToken(token)) {
        return false;
    }

    const String cuid = cfg_.cuid && cfg_.cuid[0] ? String(cfg_.cuid) : WiFi.macAddress();
    String body = "tex=" + urlEncode(normalizedText) +
                  "&tok=" + urlEncode(token) +
                  "&cuid=" + urlEncode(cuid) +
                  "&ctp=1" +
                  "&lan=" + urlEncode(String(cfg_.language ? cfg_.language : "zh")) +
                  "&spd=" + String(cfg_.speed) +
                  "&pit=" + String(cfg_.pitch) +
                  "&vol=" + String(cfg_.volume) +
                  "&per=" + String(cfg_.speaker) +
                  "&aue=" + String(cfg_.audioEncoding);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(cfg_.httpTimeoutMs);
    if (!client.connect(cfg_.ttsHost, cfg_.ttsPort)) {
        Serial.println("[BaiduTTS] ERROR: TTS TLS connect failed");
        return false;
    }

    client.print(String("POST ") + (cfg_.ttsPath ? cfg_.ttsPath : "/text2audio") + " HTTP/1.1\r\n");
    client.print(String("Host: ") + cfg_.ttsHost + "\r\n");
    client.print("Content-Type: application/x-www-form-urlencoded\r\n");
    client.print("Accept: */*\r\n");
    client.print("Accept-Encoding: identity\r\n");
    client.print("User-Agent: CuteAssistant/1.0\r\n");
    client.print(String("Content-Length: ") + body.length() + "\r\n");
    client.print("Connection: close\r\n\r\n");
    client.print(body);

    String statusLine;
    if (!readLine(client, statusLine, cfg_.httpTimeoutMs)) {
        Serial.println("[BaiduTTS] ERROR: Empty HTTP status line");
        return false;
    }

    bool chunked = false;
    int contentLength = -1;
    String contentType;
    while (true) {
        String headerLine;
        if (!readLine(client, headerLine, cfg_.httpTimeoutMs)) {
            Serial.println("[BaiduTTS] ERROR: Failed while reading response headers");
            return false;
        }
        if (headerLine.length() == 0) {
            break;
        }

        const int colon = headerLine.indexOf(':');
        if (colon <= 0) {
            continue;
        }
        String headerName = headerLine.substring(0, colon);
        String headerValue = headerLine.substring(colon + 1);
        headerName.trim();
        headerValue.trim();
        headerName.toLowerCase();
        headerValue.toLowerCase();

        if (headerName == "content-type") {
            contentType = headerValue;
        } else if (headerName == "transfer-encoding" && headerValue.indexOf("chunked") >= 0) {
            chunked = true;
        } else if (headerName == "content-length") {
            contentLength = headerValue.toInt();
        }
    }

    if (!(statusLine.indexOf(" 200 ") >= 0 || statusLine.startsWith("HTTP/1.1 200") || statusLine.startsWith("HTTP/1.0 200"))) {
        uint8_t* errorBody = nullptr;
        size_t errorBytes = 0;
        if (readBinaryBody(client, cfg_.httpTimeoutMs, chunked, contentLength, 16384, errorBody, errorBytes) && errorBody) {
            String errorText(reinterpret_cast<char*>(errorBody), errorBytes);
            Serial.print("[BaiduTTS] HTTP error body: ");
            Serial.println(errorText);
            free(errorBody);
        }
        Serial.print("[BaiduTTS] HTTP status: ");
        Serial.println(statusLine);
        return false;
    }

    if (contentType.indexOf("audio/") < 0 && contentType.indexOf("application/octet-stream") < 0) {
        uint8_t* errorBody = nullptr;
        size_t errorBytes = 0;
        if (readBinaryBody(client, cfg_.httpTimeoutMs, chunked, contentLength, 16384, errorBody, errorBytes) && errorBody) {
            String errorText(reinterpret_cast<char*>(errorBody), errorBytes);
            Serial.print("[BaiduTTS] Unexpected content-type body: ");
            Serial.println(errorText);
            free(errorBody);
        }
        Serial.print("[BaiduTTS] Unexpected content-type: ");
        Serial.println(contentType);
        return false;
    }

    if (!readBinaryBody(client,
                        cfg_.httpTimeoutMs,
                        chunked,
                        contentLength,
                        cfg_.maxAudioBytes,
                        outAudio,
                        outBytes)) {
        Serial.println("[BaiduTTS] ERROR: Failed to read audio payload");
        return false;
    }

    Serial.printf("[BaiduTTS] Synthesized %u bytes of audio for %u chars\n",
                  (unsigned)outBytes,
                  (unsigned)normalizedText.length());
    return true;
}