#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <functional>

// WiFiCredentials: holds one network entry loaded from SD
struct WiFiCredentials {
    String ssid;
    String password;
    String name;     // Optional descriptive name
    bool isValid;
};

// WiFiManager: loads multiple WiFi credentials from SD and attempts to connect.
// It also provides a helper to load API keys (e.g., OpenAI) from /apis.txt.
// Progress callback function type
typedef std::function<void(const char*)> ProgressCallback;

class WiFiManager {
private:
    WiFiCredentials networks[3]; // Up to 3 WiFi networks
    int networkCount = 0;
    int currentNetworkIndex = -1;
    bool sdMounted = false;
    ProgressCallback progressCallback = nullptr;

public:
    // Load WiFi credentials from /wifi.txt on SD (SD_MMC)
    bool loadCredentialsFromSD();

    // Try connecting to the loaded networks in order
    bool connectToWiFi();
    
    // Set progress callback for connection status updates
    void setProgressCallback(ProgressCallback callback);

    // Debug: print loaded networks
    void printLoadedNetworks();

    // Returns the name of the currently connected network (or "Disconnected")
    String getCurrentNetworkName();

    int getCurrentNetworkIndex() { return currentNetworkIndex; }
    int getNetworkCount() { return networkCount; }

    // Load API keys (OPENAI, DEEPSEEK, etc.) from /apis.txt on SD
    // Also reads optional preference value "PREFER=openai" or "PREFER=deepseek".
    bool loadApiKeysFromSD();

    // Ensure SD is mounted (idempotent). Returns true if ready
    bool ensureSDMounted();
};

// Global instance
extern WiFiManager wifiManager;

// Additional demo API configuration (exported)
extern const char* COINGECKO_API_URL;
extern const unsigned long API_UPDATE_INTERVAL;

// Global API keys loaded from SD (e.g., OPENAI key)
extern String OPENAI_API_KEY_STR;
// New Deepseek API key and preference
extern String DEEPSEEK_API_KEY_STR;
extern String API_PREFER_STR;
extern String BAIDU_ASR_AUTH_KEY_STR;
extern String BAIDU_ASR_API_KEY_STR;
extern String BAIDU_ASR_SECRET_KEY_STR;


