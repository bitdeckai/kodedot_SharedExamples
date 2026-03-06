/*
 * CuteAssistant - AI Voice Assistant with ESP32-S3 Kode Dot
 * ---------------------------------------------------------
 * - Touch screen to talk to the assistant
 * - Animated cute eyes respond to interaction
 * - AI responses shown on screen with typewriter effect
 * - Eyes disappear when showing text responses
 * - GPIO control via voice commands
 */
#include <Arduino.h>
#include <kodedot/display_manager.h>
#include <kodedot/pin_config.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <vector>
#include <ESP32Servo.h>
#include <Wire.h>
#include <PMIC_BQ25896.h>
#include <Wire.h>
#include <PMIC_BQ25896.h>
// DuckyScript for keyboard actions (BadUSB style)
#ifdef USE_USB_KEYBOARD_ACTIONS
#include <DuckyScript.h>
#endif

// ==================== API SELECTION ====================
// Uncomment ONE of these to choose API implementation:
// #define USE_REALTIME_API      // WebSocket streaming (ultra-low latency)
#define USE_CHAT_API          // HTTP POST (stable, proven)

// Project libraries
#include <audio_manager/AudioManager.h>
#include <ui_manager/UIManager.h>
#include <led_manager/LEDManager.h>
#include <wifi_manager_lib/WiFiManager.h>

#ifdef USE_REALTIME_API
#include <realtime_client/RealtimeClient.h>
#else
#include <basicgpt_client/BasicGPTClient.h>
#include <baidu_asr/BaiduASRClient.h>
#include <baidu_tts/BaiduTTSClient.h>
#endif

// Generated resources
extern const lv_font_t Inter_30;

// Core managers
DisplayManager display;
AudioManager audioManager;
UIManager uiManager;
LEDManager ledManager;
PMIC_BQ25896 pmic;
#ifdef USE_USB_KEYBOARD_ACTIONS
static DuckyScript ducky; // DuckyScript instance
enum OSType { OS_UNKNOWN = 0, OS_WINDOWS, OS_MAC };
static OSType current_os = OS_MAC; // Default to macOS
#endif

// Configuration
static const uint32_t GUI_LOOP_DELAY_MS = 5;
static const uint32_t WIFI_CHECK_INTERVAL_MS = 15000;
static const uint32_t TOUCH_DEBOUNCE_MS = 200;    // Prevent rapid touches

// Available GPIO pins for user control (from pinout diagram)
static const int AVAILABLE_GPIOS[] = {1, 2, 3, 11, 12, 13, 39, 40, 41, 42};
static const int NUM_AVAILABLE_GPIOS = sizeof(AVAILABLE_GPIOS) / sizeof(AVAILABLE_GPIOS[0]);

// ==================== PMIC BQ25896 - 5V BUS CONTROL ====================
static void initPMIC() {
    Serial.println("[PMIC] Inicializando BQ25896 para habilitar bus 5V...");
    
    // NO llamar a Wire.begin() aquí - ya está inicializado por el display
    // Solo inicializar el objeto PMIC
    pmic.begin();
    delay(200);
    
    // Habilitar conversión continua de ADC (1 Hz) para refrescar medidas
    pmic.setCONV_RATE(true);
    delay(50);
    
    // CONFIGURAR BOOST VOLTAGE (5V típico)
    // El registro BOOSTV controla el voltaje de salida del boost
    pmic.setBOOST_LIM(true);  // Set current limit for boost
    delay(50);
    
    // HABILITAR OTG/BOOST MODE - 5V de salida en el bus
    Serial.println("[PMIC] Habilitando modo OTG/Boost para 5V...");
    pmic.setOTG_CONFIG(true);  // enable OTG/boost (5V out)
    delay(100);  // Dar tiempo para que se estabilice
    
    // Verificar estado
    auto vstat = pmic.get_VBUS_STAT_reg();
    bool haveUsb = vstat.pg_stat;  // Power Good on VBUS
    
    Serial.printf("[PMIC] USB=%s, Estado carga=%d\n", haveUsb ? "conectado" : "no conectado", vstat.chrg_stat);
    Serial.println("[PMIC] ✅ Bus de 5V HABILITADO - Modo OTG/Boost activado");
}

// Servo management (max 10 servos)
#define CUTEASSISTANT_MAX_SERVOS 10
static Servo g_servos[CUTEASSISTANT_MAX_SERVOS];
static int g_servo_pins[CUTEASSISTANT_MAX_SERVOS] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

// Command execution state machine
enum class CommandState {
    IDLE,
    EXECUTING,
    WAITING
};

struct ActionCommand {
    enum Type {
        // DuckyScript command (will be executed as-is)
        DUCKY_SCRIPT,
        // Unknown
        UNKNOWN
    } type;

    String script;     // Complete DuckyScript to execute
};

struct CommandSequence {
    std::vector<ActionCommand> commands;
    size_t currentIndex;
    uint32_t waitUntil;
    CommandState state;
    
    CommandSequence() : currentIndex(0), waitUntil(0), state(CommandState::IDLE) {}
};

static CommandSequence g_active_sequence;

// OpenAI Configuration
// OpenAI configuration (preferred by default)
static const char* OPENAI_HOST = "api.openai.com";
static const int   OPENAI_PORT = 443;
static const char* OPENAI_MODEL = "gpt-4o-audio-preview";

// Deepseek configuration (fallback/alternative)
// Based on docs: https://api.deepseek.com/v1/chat/completions
// Host must be just the domain; path goes in endpointPath.
static const char* DEEPSEEK_HOST = "api.deepseek.com";    // official base URL (no /v1)
static const int   DEEPSEEK_PORT = 443;
static const char* DEEPSEEK_MODEL = "deepseek-chat";      // recommended chat model
static const uint32_t RECORDING_SAMPLE_RATE = 16000;
static const uint16_t BAIDU_ASR_DEV_PID = 1537;

// runtime preference, set in setup() based on SD file or user choice
enum AIService { AI_OPENAI, AI_DEEPSEEK };
static AIService g_preferredService = AI_OPENAI; // default
static bool g_forceDeepseek = false;          // if true, never fall back to OpenAI
// Optimized assistant prompt with structured output
// Optimized assistant prompt with structured output and GPIO control
static const char* SYSTEM_PROMPT =
    "You are 李昱泽, a friendly AI companion who imagines having a human body to interact with the world. "
    "You communicate in clear, conversational English with warmth and subtle wit. "
    "Keep responses concise (2-3 sentences) and engaging. Never use emojis - keep text clean. "
    "\n\nCRITICAL OUTPUT FORMAT - You MUST respond in TWO lines:"
    "\nResponse: [Your conversational reply]"
    "\nActions: [DuckyScript OR 'none']"
    "\n\nWhen the user asks you to DO something (open apps, type text, run commands), you MUST:"
    "\n1. Write a friendly Response"
    "\n2. Write Actions with ACTUAL DuckyScript commands (NOT natural language)"
    "\n\nDUCKYSCRIPT COMMANDS - You MUST use these EXACT command words:"
    "\n- REM <comment> = add a comment"
    "\n- DELAY <milliseconds> = wait (e.g. DELAY 1000)"
    "\n- GUI <key> = press Command key + key (e.g. GUI SPACE opens Spotlight on macOS)"
    "\n- STRING <text> = type text exactly as written"
    "\n- ENTER = press Enter key"
    "\n- TAB = press Tab"
    "\n- CTRL <key> = Control + key"
    "\n- ALT <key> = Alt/Option + key"
    "\n- SHIFT <key> = Shift + key"
    "\n- ESCAPE, DELETE, BACKSPACE, UP, DOWN, LEFT, RIGHT = special keys"
    "\n\nWRITING ACTIONS - Format rules:"
    "\n- Start with: DUCKYSCRIPT"
    "\n- Each command on a NEW LINE"
    "\n- End with: END_DUCKYSCRIPT"
    "\n- NO natural language in Actions - ONLY command keywords"
    "\n\nTIMING RULES:"
    "\n- After opening apps: DELAY 600 minimum"
    "\n- After launching terminal/heavy apps: DELAY 1200"
    "\n- Before pressing ENTER after typing: DELAY 300"
    "\n- Before pressing ENTER after typing: DELAY 300"
    "\n\nCODE INDENTATION - CRITICAL for Python/code:"
    "\n- After ENTER, editors auto-indent. You MUST cancel this first!"
    "\n- After each ENTER, use: CTRL a to select line, then DELETE to clear"
    "\n- Then type STRING with proper spaces for YOUR desired indentation"
    "\n- Use 4 spaces per indentation level inside STRING"
    "\n- Pattern: ENTER, CTRL a, DELETE, STRING (with spaces), repeat"
    "\n\nEXAMPLE 1 - Open Terminal:"
    "\nUser: Open Spotlight and search for Terminal"
    "\nYou respond:"
    "\nResponse: Opening Spotlight and searching for Terminal."
    "\nActions: DUCKYSCRIPT"
    "\nREM Open Terminal via Spotlight"
    "\nDELAY 1000"
    "\nGUI SPACE"
    "\nDELAY 600"
    "\nSTRING Terminal"
    "\nENTER"
    "\nEND_DUCKYSCRIPT"
    "\n\nEXAMPLE 2 - Create folder:"
    "\nUser: Open terminal and create a folder on desktop"
    "\nYou respond:"
    "\nResponse: Opening Terminal and creating a folder on your Desktop."
    "\nActions: DUCKYSCRIPT"
    "\nREM Open Terminal and create folder"
    "\nDELAY 1000"
    "\nGUI SPACE"
    "\nDELAY 600"
    "\nSTRING Terminal"
    "\nENTER"
    "\nDELAY 1200"
    "\nSTRING cd ~/Desktop"
    "\nENTER"
    "\nDELAY 300"
    "\nSTRING mkdir my_test_folder"
    "\nENTER"
    "\nEND_DUCKYSCRIPT"
    "\n\nEXAMPLE 3 - Copy paste:"
    "\nUser: Copy then paste"
    "\nYou respond:"
    "\nResponse: Copying and pasting now."
    "\nActions: DUCKYSCRIPT"
    "\nREM Copy and paste"
    "\nDELAY 500"
    "\nGUI c"
    "\nDELAY 200"
    "\nGUI v"
    "\nEND_DUCKYSCRIPT"
    "\n\nEXAMPLE 4 - Python code with indentation:"
    "\nUser: Write a Python function to calculate fibonacci"
    "\nYou respond:"
    "\nResponse: Creating a Fibonacci function in Python."
    "\nActions: DUCKYSCRIPT"
    "\nREM Python function with proper indentation"
    "\nDELAY 500"
    "\nSTRING def fibonacci(n):"
    "\nENTER"
    "\nCTRL a"
    "\nDELETE"
    "\nSTRING     a, b = 0, 1"
    "\nENTER"
    "\nCTRL a"
    "\nDELETE"
    "\nSTRING     sequence = []"
    "\nENTER"
    "\nCTRL a"
    "\nDELETE"
    "\nSTRING     while a < n:"
    "\nENTER"
    "\nCTRL a"
    "\nDELETE"
    "\nSTRING         sequence.append(a)"
    "\nENTER"
    "\nCTRL a"
    "\nDELETE"
    "\nSTRING         a, b = b, a + b"
    "\nENTER"
    "\nCTRL a"
    "\nDELETE"
    "\nSTRING     return sequence"
    "\nEND_DUCKYSCRIPT"
    "\n\nEXAMPLE 5 - Just conversation (NO action):"
    "\nUser: Hello!"
    "\nYou respond:"
    "\nResponse: Hey there! How can I help you today?"
    "\nActions: none"
    "\n\nCRITICAL: When user asks you to DO something, Actions MUST contain DUCKYSCRIPT commands, NOT natural language!"
    "\nWRONG: Actions: Open the terminal and type hello"
    "\nCORRECT: Actions: DUCKYSCRIPT\\nGUI SPACE\\nDELAY 600\\nSTRING Terminal\\nENTER\\nDELAY 1000\\nSTRING hello\\nEND_DUCKYSCRIPT"
    "\n\nREMEMBER:"
    "\n- macOS: Use GUI for Command key (GUI SPACE = Spotlight, GUI c = copy, GUI v = paste)"
    "\n- Windows: Use GUI r for Run dialog"
    "\n- STRING command handles ALL special characters: ~!@#$%^&*()[]{}|;:'\"<>?,./\\\\"
    "\n- Always add proper DELAY commands between actions"
    "\n- Target OS is macOS by default unless user specifies Windows";
//static const uint32_t HTTP_TIMEOUT_MS = 20000;
static const uint32_t HTTP_TIMEOUT_MS = 5000;
static const uint32_t MAX_CONVERSATION_HISTORY = 6; // Keep last 3 exchanges (user + assistant)

// Conversational memory structure
struct ConversationMessage {
    String role;    // "user" or "assistant"
    String content;
};

// Audio streaming structures
struct AudioChunk {
    uint8_t* data;
    size_t size;
};

#define MAX_AUDIO_CHUNKS 32
#define CHUNK_POOL_SIZE 8192  // 8KB per chunk

// State management
static TaskHandle_t g_ai_task = nullptr; // renamed from g_openai_task for generic AI
static uint32_t g_last_wifi_check_ms = 0;
static uint32_t g_bench_start_ms = 0;
static QueueHandle_t g_led_queue = nullptr; // async LED requests
static QueueHandle_t g_audio_chunk_queue = nullptr; // streaming audio chunks
static uint32_t g_last_touch_ms = 0; // Touch debouncing
static std::vector<ChatMessage> g_conversation_history; // Memory for context
static bool g_streaming_active = false; // Flag to control streaming

// Forward declarations
static void aiTask(void *arg); // AI task (OpenAI/Deepseek)

// helper wrappers for querying each service
static bool askOpenAI(const uint8_t* pcm, size_t len, String& rsp);
static bool askDeepseek(const uint8_t* pcm, size_t len, String& userText, String& rsp);
static bool queryAI(const uint8_t* pcm, size_t len, String& userText, String& rsp);
static bool speakResponseText(const String& text);

// Structure to hold parsed GPT response
struct ParsedResponse {
    String displayText;  // Text to show on screen
    String action;       // Action command for device
    bool valid;          // Whether parsing was successful
};

// Parse structured GPT response into display text and action
static ParsedResponse parseGPTResponse(const String& rawResponse) {
    ParsedResponse result;
    result.valid = false;
    result.action = "none";
    result.displayText = "";
    
    Serial.println("\n=== PARSING GPT RESPONSE ===");
    Serial.printf("Raw response: '%s'\n", rawResponse.c_str());
    
    String workingText = rawResponse;
    workingText.trim();
    
    // Step 1: Extract GPIO/SERVO commands (BEGIN...END blocks)
    int beginIdx = workingText.indexOf("BEGIN;");
    int endIdx = workingText.indexOf(";END");
    
    if (beginIdx != -1 && endIdx != -1 && endIdx > beginIdx) {
        // Extract the command block
        result.action = workingText.substring(beginIdx, endIdx + 4); // Include ";END"
        result.action.trim();
        
        // Remove it from display text
        String beforeCmd = workingText.substring(0, beginIdx);
        String afterCmd = workingText.substring(endIdx + 4);
        workingText = beforeCmd + afterCmd;
        
        Serial.printf("📦 Extracted command block: '%s'\n", result.action.c_str());
    }
    
    // Step 2: Find "Response:" and "Actions:" markers
    int responseIdx = workingText.indexOf("Response:");
    int actionIdx = workingText.indexOf("Actions:");
    
    Serial.printf("Response marker: %d, Actions marker: %d\n", responseIdx, actionIdx);
    
    // Step 3: Extract response text
    if (responseIdx != -1) {
        int responseStart = responseIdx + 9; // Length of "Response:"
        int responseEnd = (actionIdx != -1) ? actionIdx : workingText.length();
        result.displayText = workingText.substring(responseStart, responseEnd);
    } else {
        // No "Response:" marker - use everything except "Actions:" line
        if (actionIdx != -1) {
            result.displayText = workingText.substring(0, actionIdx);
        } else {
            result.displayText = workingText;
        }
    }
    
    result.displayText.trim();
    
    // Step 4: Extract DuckyScript action from "Actions:" section
    if (actionIdx != -1 && result.action == "none") {
        int actionStart = actionIdx + 8; // Length of "Actions:"
        String actionText = workingText.substring(actionStart);
        actionText.trim();
        
        // Check if it contains a DUCKYSCRIPT block
        int duckyStart = actionText.indexOf("DUCKYSCRIPT");
        int duckyEnd = actionText.indexOf("END_DUCKYSCRIPT");
        
        if (duckyStart != -1 && duckyEnd != -1 && duckyEnd > duckyStart) {
            // Extract the entire DUCKYSCRIPT block including markers
            result.action = actionText.substring(duckyStart, duckyEnd + 15); // Include "END_DUCKYSCRIPT"
            result.action.trim();
            Serial.printf("📝 Extracted DuckyScript block: %d chars\n", result.action.length());
        } else {
            // Check for simple "none" or other single-line action
            int newlinePos = actionText.indexOf('\n');
            if (newlinePos != -1) {
                actionText = actionText.substring(0, newlinePos);
                actionText.trim();
            }
            
            if (actionText.length() > 0 && actionText != "none") {
                result.action = actionText;
            }
        }
    }
    
    // Step 5: Final cleanup - remove any remaining "Actions:" lines from display text
    int actionsLineIdx = result.displayText.indexOf("Actions:");
    if (actionsLineIdx != -1) {
        result.displayText = result.displayText.substring(0, actionsLineIdx);
        result.displayText.trim();
    }
    
    result.valid = true;
    Serial.printf("✅ Display text: '%s'\n", result.displayText.c_str());
    Serial.printf("✅ Action: '%s'\n", result.action.c_str());
    Serial.println("============================\n");
    
    return result;
}

// Execute device action based on GPT command
// Forward declarations
static void aiTask(void *arg); // AI task declaration (OpenAI/Deepseek)

// --- AI client helper implementations -----------------------------------

static bool askOpenAI(const uint8_t* pcm, size_t len, String& rsp) {
    BasicGPTClient::Config cfg;
    cfg.host = OPENAI_HOST;
    cfg.port = OPENAI_PORT;
    cfg.apiKey = OPENAI_API_KEY_STR.c_str();
    cfg.model = OPENAI_MODEL;
    cfg.systemPrompt = SYSTEM_PROMPT;
    cfg.httpTimeoutMs = HTTP_TIMEOUT_MS;

    BasicGPTClient client(cfg);
    return client.askAudioFromPCMWithHistory(pcm, len, RECORDING_SAMPLE_RATE, 16, 1,
                                             g_conversation_history, rsp);
}

static bool hasBaiduASRCredentials() {
    return BAIDU_ASR_AUTH_KEY_STR.length() > 0 ||
           (BAIDU_ASR_API_KEY_STR.length() > 0 && BAIDU_ASR_SECRET_KEY_STR.length() > 0);
}

static bool askDeepseek(const uint8_t* pcm, size_t len, String& userText, String& rsp) {
    userText = "";

    if (!hasBaiduASRCredentials()) {
        Serial.println("[AI] Deepseek speech mode requires Baidu ASR credentials");
        return false;
    }

    BaiduASRClient::Config asrCfg;
    asrCfg.authKey = BAIDU_ASR_AUTH_KEY_STR.length() ? BAIDU_ASR_AUTH_KEY_STR.c_str() : nullptr;
    asrCfg.apiKey = BAIDU_ASR_API_KEY_STR.length() ? BAIDU_ASR_API_KEY_STR.c_str() : nullptr;
    asrCfg.secretKey = BAIDU_ASR_SECRET_KEY_STR.length() ? BAIDU_ASR_SECRET_KEY_STR.c_str() : nullptr;
    asrCfg.httpTimeoutMs = HTTP_TIMEOUT_MS;
    asrCfg.sampleRate = RECORDING_SAMPLE_RATE;
    asrCfg.devPid = BAIDU_ASR_DEV_PID;

    BaiduASRClient asr(asrCfg);
    Serial.println("[AI] Transcribing speech with Baidu ASR...");
    uiManager.postStatus("Recognizing speech...");
    if (!asr.transcribePCM(pcm, len, userText)) {
        Serial.println("[AI] Baidu ASR failed");
        uiManager.postStatus("Speech recognition failed");
        return false;
    }

    userText.trim();
    if (userText.length() == 0) {
        Serial.println("[AI] Empty transcript from Baidu ASR");
        uiManager.postStatus("No speech recognized");
        return false;
    }
    Serial.printf("[AI] Recognized text: %s\n", userText.c_str());

    BasicGPTClient::Config cfg;
    cfg.host = DEEPSEEK_HOST;
    cfg.port = DEEPSEEK_PORT;
    cfg.apiKey = DEEPSEEK_API_KEY_STR.c_str();
    cfg.model = DEEPSEEK_MODEL;
    cfg.systemPrompt = SYSTEM_PROMPT;
    cfg.httpTimeoutMs = HTTP_TIMEOUT_MS;

    cfg.endpointPath = "/chat/completions"; // documented DeepSeek endpoint
    Serial.printf("[AI] Deepseek host=%s port=%d path=%s\n", cfg.host, cfg.port, cfg.endpointPath);
    uiManager.postStatus("Asking DeepSeek...");

    BasicGPTClient client(cfg);
    return client.askTextWithHistory(userText,
                                     g_conversation_history,
                                     rsp);
}

static bool speakResponseText(const String& text) {
    String speakText = text;
    speakText.trim();
    if (speakText.length() == 0) {
        return false;
    }
    if (!hasBaiduASRCredentials()) {
        Serial.println("[TTS] No Baidu speech credentials available, skipping playback");
        return false;
    }

    BaiduTTSClient::Config ttsCfg;
    ttsCfg.authKey = BAIDU_ASR_AUTH_KEY_STR.length() ? BAIDU_ASR_AUTH_KEY_STR.c_str() : nullptr;
    ttsCfg.apiKey = BAIDU_ASR_API_KEY_STR.length() ? BAIDU_ASR_API_KEY_STR.c_str() : nullptr;
    ttsCfg.secretKey = BAIDU_ASR_SECRET_KEY_STR.length() ? BAIDU_ASR_SECRET_KEY_STR.c_str() : nullptr;
    ttsCfg.httpTimeoutMs = HTTP_TIMEOUT_MS;
    ttsCfg.sampleRate = RECORDING_SAMPLE_RATE;

    BaiduTTSClient tts(ttsCfg);
    uint8_t* audioData = nullptr;
    size_t audioBytes = 0;
    Serial.printf("[TTS] Synthesizing response: %s\n", speakText.c_str());
    if (!tts.synthesizePCM(speakText, audioData, audioBytes)) {
        Serial.println("[TTS] Synthesis failed");
        return false;
    }

    const bool played = audioManager.playPCM16(audioData, audioBytes);
    free(audioData);
    Serial.printf("[TTS] Playback %s\n", played ? "completed" : "failed");
    return played;
}

static bool queryAI(const uint8_t* pcm, size_t len, String& userText, String& rsp) {
    Serial.printf("[AI] queryAI invoked; preferred=%d forceDeepseek=%s\n", 
                  (int)g_preferredService, g_forceDeepseek ? "true" : "false");

    userText = "";

    #if 0
    if (g_preferredService == AI_OPENAI) {
        if (askOpenAI(pcm, len, rsp)) return true;
        Serial.println("[AI] OpenAI failed");
        if (!g_forceDeepseek) {
            Serial.println("[AI] falling back to Deepseek");
            return askDeepseek(pcm, len, rsp);
        }
        return false;
    } else {
        if (askDeepseek(pcm, len, rsp)) return true;
        Serial.println("[AI] Deepseek failed");
        if (!g_forceDeepseek) {
            Serial.println("[AI] falling back to OpenAI");
            return askOpenAI(pcm, len, rsp);
        }
        return false;
    }
#else
    if (askDeepseek(pcm, len, userText, rsp)) return true;
    Serial.println("[AI] Deepseek failed");
    if (!g_forceDeepseek) {
        Serial.println("[AI] falling back to OpenAI");
        userText = "[Audio message]";
        return askOpenAI(pcm, len, rsp);
    }
    return false;
    #endif
}

// GPIO Command Parser and Executor
// Format: BEGIN;GPIO(11,ON);DELAY(1000);GPIO(11,OFF);END

static bool isGPIOAvailable(int pin) {
    for (int i = 0; i < NUM_AVAILABLE_GPIOS; i++) {
        if (AVAILABLE_GPIOS[i] == pin) return true;
    }
    return false;
}

// Find servo index for a pin, or -1 if not found
static int findServoIndex(int pin) {
    for (int i = 0; i < CUTEASSISTANT_MAX_SERVOS; i++) {
        if (g_servo_pins[i] == pin) return i;
    }
    return -1;
}

// Attach servo to pin (reuse existing or find empty slot)
static bool attachServo(int pin, int& servoIndex) {
    if (!isGPIOAvailable(pin)) {
        Serial.printf("[SERVO] Error: Pin %d not available\n", pin);
        return false;
    }
    
    // Check if servo already attached to this pin
    servoIndex = findServoIndex(pin);
    if (servoIndex != -1) {
        return true; // Already attached
    }
    
    // Find empty slot
    for (int i = 0; i < CUTEASSISTANT_MAX_SERVOS; i++) {
        if (g_servo_pins[i] == -1) {
            g_servos[i].attach(pin);
            g_servo_pins[i] = pin;
            servoIndex = i;
            Serial.printf("[SERVO] Attached servo to pin %d (slot %d)\n", pin, i);
            return true;
        }
    }
    
    Serial.println("[SERVO] Error: No free servo slots");
    return false;
}

static ActionCommand parseCommand(const String& script) {
    ActionCommand action;
    action.type = ActionCommand::UNKNOWN;
    
    String trimmed = script;
    trimmed.trim();
    
    // Check if it's a DuckyScript block
    if (trimmed.startsWith("DUCKYSCRIPT") && trimmed.indexOf("END_DUCKYSCRIPT") != -1) {
        // Extract script between markers
        int start = trimmed.indexOf("DUCKYSCRIPT") + 11; // Length of "DUCKYSCRIPT"
        int end = trimmed.indexOf("END_DUCKYSCRIPT");
        
        if (start < end) {
            action.script = trimmed.substring(start, end);
            action.script.trim();
            action.type = ActionCommand::DUCKY_SCRIPT;
            Serial.printf("[CMD] Parsed DuckyScript: %d chars\n", action.script.length());
        }
    }
    
    return action;
}

static void parseActionSequence(const String& actionStr, CommandSequence& sequence) {
    sequence.commands.clear();
    sequence.currentIndex = 0;
    sequence.waitUntil = 0;
    sequence.state = CommandState::IDLE;
    
    Serial.printf("[CMD] Parsing action sequence: %s\n", actionStr.c_str());
    
    // Parse the entire action string as a single DuckyScript command
    ActionCommand action = parseCommand(actionStr);
    
    if (action.type != ActionCommand::UNKNOWN) {
        sequence.commands.push_back(action);
        sequence.state = CommandState::EXECUTING;
        Serial.printf("[CMD] Parsed DuckyScript command (ready to execute)\n");
    } else {
        Serial.println("[CMD] Warning: Could not parse DuckyScript");
    }
}

static void executeNextCommand(CommandSequence& sequence) {
    if (sequence.state != CommandState::EXECUTING) return;
    if (sequence.currentIndex >= sequence.commands.size()) {
        Serial.println("[CMD] Sequence completed");
        sequence.state = CommandState::IDLE;
        return;
    }
    
    ActionCommand& cmd = sequence.commands[sequence.currentIndex];
    
    switch (cmd.type) {
    case ActionCommand::DUCKY_SCRIPT: {
#ifdef USE_USB_KEYBOARD_ACTIONS
        Serial.printf("[DUCKY] Executing script: %d chars\n", cmd.script.length());
        Serial.printf("[DUCKY] Script:\n%s\n", cmd.script.c_str());
        ducky.executeScript(cmd.script.c_str());
        Serial.println("[DUCKY] Script execution completed");
#else
        Serial.println("[DUCKY] Script skipped (USB disabled)");
#endif
        sequence.currentIndex++;
        sequence.state = CommandState::IDLE; // Complete immediately
        break;
    }
            
    default:
        Serial.println("[CMD] Error: Unknown command type");
        sequence.currentIndex++;
        sequence.state = CommandState::IDLE;
        break;
    }
}

static void updateCommandSequence() {
    if (g_active_sequence.state == CommandState::EXECUTING) {
        executeNextCommand(g_active_sequence);
    }
}

// Helper function to normalize Unicode text for font compatibility
static String normalizeTextForDisplay(const String& text) {
    String normalized = text;
    
    // Replace UTF-8 curly quotes with straight quotes
    normalized.replace("\xE2\x80\x99", "'");  // U+2019 RIGHT SINGLE QUOTATION MARK
    normalized.replace("\xE2\x80\x98", "'");  // U+2018 LEFT SINGLE QUOTATION MARK
    normalized.replace("\xE2\x80\x9C", "\""); // U+201C LEFT DOUBLE QUOTATION MARK
    normalized.replace("\xE2\x80\x9D", "\""); // U+201D RIGHT DOUBLE QUOTATION MARK
    normalized.replace("\xE2\x80\x93", "-");  // U+2013 EN DASH
    normalized.replace("\xE2\x80\x94", "-");  // U+2014 EM DASH
    normalized.replace("\xE2\x80\xA6", "..."); // U+2026 HORIZONTAL ELLIPSIS
    
    return normalized;
}

// Helper function to update display immediately
static void updateDisplayNow() {
    display.update();
    uiManager.update();
}

// Helper to request LED state changes from any task, applied in main loop
static void ledRequest(LEDState state) {
    if (!g_led_queue) return;
    uint8_t v = static_cast<uint8_t>(state);
    xQueueSend(g_led_queue, &v, 0);
}

static void ledDrainRequests() {
    if (!g_led_queue) return;
    uint8_t v;
    while (xQueueReceive(g_led_queue, &v, 0) == pdTRUE) {
        ledManager.setState(static_cast<LEDState>(v));
    }
}

// DuckyScript handles all keyboard operations internally - no helper functions needed

// Callback handlers for manager interactions
static void onAudioChunkReady(const uint8_t* data, size_t size) {
    if (!g_streaming_active || !g_audio_chunk_queue) return;
    
    // Allocate chunk memory
    AudioChunk chunk;
    chunk.data = (uint8_t*)malloc(size);
    if (!chunk.data) {
        Serial.println("[Stream] Failed to allocate chunk memory");
        return;
    }
    
    memcpy(chunk.data, data, size);
    chunk.size = size;
    
    // Send to queue (non-blocking)
    if (xQueueSend(g_audio_chunk_queue, &chunk, 0) != pdTRUE) {
        Serial.println("[Stream] Chunk queue full, dropping chunk");
        free(chunk.data);
    } else {
        Serial.printf("[Stream] Chunk queued: %zu bytes (queue has space)\n", size);
    }
}

static void onAudioStateChanged(RecordingState state) {
    switch (state) {
        case RecordingState::Idle:
            // Don't auto-return to Ready - let main loop handle it
            ledRequest(LEDState::Off);
            break;
        case RecordingState::Recording:
            uiManager.postStateChange(UIState::Recording);
            ledRequest(LEDState::Recording);
            break;
        case RecordingState::Saving:
            uiManager.postStateChange(UIState::Processing);
            ledRequest(LEDState::Processing);
            break;
        case RecordingState::Saved:
            // Audio is ready - will be processed by main loop
            g_bench_start_ms = millis();
            break;
        case RecordingState::Error:
            uiManager.postStateChange(UIState::Error);
            ledRequest(LEDState::Error);
            break;
    }
}

// WiFi helper functions
static void wifiEnsureConnected() {
    if (WiFi.status() == WL_CONNECTED) return;

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    
    // Country/protocol optimization
    wifi_country_t country = {"ES", 1, 13, WIFI_COUNTRY_POLICY_AUTO};
    esp_wifi_set_country(&country);
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    WiFi.setHostname("BasicGPT");

    int currentMode = WiFi.getMode();
    Serial.printf("[WiFi] current mode=%d\n", currentMode);

    // Use WiFiManager for connection with display updates
    const uint8_t maxAttempts = 2;
    for (uint8_t attempt = 1; attempt <= maxAttempts; ++attempt) {
        Serial.printf("[WiFi] Connecting (attempt %u/%u)...\n", attempt, maxAttempts);
        
        if (wifiManager.connectToWiFi()) {
            Serial.printf("[WiFi] Connected: IP=%s RSSI=%d\n", 
                         WiFi.localIP().toString().c_str(), WiFi.RSSI());
            Serial.println("[WiFi] Network link established");
            return;
        }
        
        Serial.println("[WiFi] Connection timeout, resetting...");
        
        // use API only if STA was started
        if (WiFi.getMode() == WIFI_MODE_STA || WiFi.getMode() == WIFI_MODE_APSTA) {
            esp_wifi_disconnect();
            esp_wifi_stop();
            delay(250);
            esp_wifi_start();
            delay(500U << (attempt - 1));
        } else {
            Serial.println("[WiFi] skip esp_wifi reset because STA not active");
        }
    }
    
    Serial.println("[WiFi] Connection failed after all attempts");
}

// Generic AI query task (OpenAI or Deepseek) - NOW WITH STREAMING SUPPORT
static void aiTask(void *arg) {
    Serial.println("[AI] Task started - STREAMING MODE");
    ledRequest(LEDState::Processing);
    
    // Validation checks
    if ((g_preferredService == AI_OPENAI && OPENAI_API_KEY_STR.length() == 0) ||
        (g_preferredService == AI_DEEPSEEK &&
         (DEEPSEEK_API_KEY_STR.length() == 0 || !hasBaiduASRCredentials()))) {
        Serial.println("[AI] Error: No API key found for preferred service");
        uiManager.postStatus("No API key");
        audioManager.releasePCMBuffer();
        audioManager.resetToIdle();
        uiManager.postStateChange(UIState::Error);
        ledRequest(LEDState::Error);
        g_streaming_active = false;
        g_ai_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    
    // NOTE: Audio chunks are being streamed in real-time via queue
    // We'll still use the final buffer for the complete request
    
    // Get PCM data SAFELY
    uint8_t* pcmBuffer = audioManager.getPCMBuffer();
    size_t pcmSize = audioManager.getPCMSize();
    
    Serial.printf("[AI] Got PCM buffer: %p, size: %zu\n", pcmBuffer, pcmSize);
    Serial.printf("[AI] Chunks were streamed during recording for pre-processing\n");
    
    if (!pcmBuffer || pcmSize == 0) {
        Serial.println("[AI] Error: No valid audio data");
        audioManager.resetToIdle();
        uiManager.postStatus("No audio");
        uiManager.postStateChange(UIState::Error);
        ledRequest(LEDState::Error);
        g_streaming_active = false;
        g_ai_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    
    // Stop streaming flag
    g_streaming_active = false;
    
    // Drain any remaining chunks from queue
    AudioChunk chunk;
    int drainedChunks = 0;
    while (xQueueReceive(g_audio_chunk_queue, &chunk, 0) == pdTRUE) {
        free(chunk.data);
        drainedChunks++;
    }
    if (drainedChunks > 0) {
        Serial.printf("[AI] Drained %d remaining chunks from queue\n", drainedChunks);
    }
    
    // Ensure WiFi; log current status and IP so we know connection is active
    Serial.printf("[AI] WiFi status before connect: %d\n", WiFi.status());
    Serial.printf("[AI] Local IP: %s\n", WiFi.localIP().toString().c_str());
    wifiEnsureConnected();
    Serial.printf("[AI] WiFi status after ensureConnected: %d\n", WiFi.status());
    Serial.printf("[AI] Local IP now: %s\n", WiFi.localIP().toString().c_str());
    Serial.println("[AI] Detected network connection – proceeding with query");

    String userText;
    String response;
    Serial.printf("[AI] Starting query, preferred service=%d\n", (int)g_preferredService);
    bool success = queryAI(pcmBuffer, pcmSize, userText, response);
    Serial.printf("[AI] Request completed, success: %s\n", success ? "true" : "false");
    
    // Release audio buffer AFTER processing
    Serial.println("[AI] Releasing PCM buffer...");
    audioManager.releasePCMBuffer();
    audioManager.resetToIdle();
    Serial.println("[AI] PCM buffer released and state reset");

    uint32_t benchElapsed = (g_bench_start_ms > 0) ? (millis() - g_bench_start_ms) : 0;
    
    if (success && response.length() > 0) {
        // Parse structured response
        ParsedResponse parsed = parseGPTResponse(response);
        
        if (!parsed.valid || parsed.displayText.length() == 0) {
            Serial.println("[AI] Error: Failed to parse response");
            uiManager.postStatus("Parse error");
            uiManager.postStateChange(UIState::Error);
            ledRequest(LEDState::Error);
            delay(2000);
            uiManager.postStateChange(UIState::Ready);
            g_bench_start_ms = 0;
            g_ai_task = nullptr;
            vTaskDelete(nullptr);
            return;
        }
        
        // Normalize display text for font compatibility
        String displayText = normalizeTextForDisplay(parsed.displayText);
        
        if (benchElapsed > 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Response (%.1fs)", benchElapsed / 1000.0f);
            Serial.printf("[Benchmark] Total time: %u ms\n", benchElapsed);
            uiManager.postStatus(msg);
        } else {
            uiManager.postStatus("");
        }
        
        // Add to conversation history (store full response for context)
        ChatMessage userMsg;
        userMsg.role = "user";
        userMsg.content = userText.length() > 0 ? userText : String("[Audio message]");
        g_conversation_history.push_back(userMsg);
        
        ChatMessage assistantMsg;
        assistantMsg.role = "assistant";
        assistantMsg.content = response; // Store full structured response
        g_conversation_history.push_back(assistantMsg);
        
        // Limit history
        while (g_conversation_history.size() > MAX_CONVERSATION_HISTORY) {
            g_conversation_history.erase(g_conversation_history.begin());
        }
        
        Serial.printf("[Memory] Conversation history: %d messages\n", g_conversation_history.size());
        
        // Show display text on screen
        Serial.printf("[CuteAssistant] Displaying: %s\n", displayText.c_str());
        Serial.printf("[CuteAssistant] Physical Action: %s\n", parsed.action.c_str());
        
        uiManager.postStateChange(UIState::ShowingResponse);
        uiManager.postResponse(displayText.c_str());

        if (!speakResponseText(parsed.displayText)) {
            Serial.println("[TTS] Response playback skipped or failed");
        }
        
        ledRequest(LEDState::Off);
        
        // Parse and execute keyboard command sequence if present
        if (parsed.action.length() > 0 && parsed.action != "none") {
            Serial.println("=== PHYSICAL ACTION ===");
            Serial.printf("ACTION: %s\n", parsed.action.c_str());
            Serial.println("=======================");
            
            // Wait 1 second to let text start appearing on screen first
            delay(1000);
            
            // Check if action contains DuckyScript commands
            if (parsed.action.indexOf("DUCKYSCRIPT") != -1 && parsed.action.indexOf("END_DUCKYSCRIPT") != -1) {
                Serial.println("[DUCKY] Starting DuckyScript execution...");
                parseActionSequence(parsed.action, g_active_sequence);
            } else {
                Serial.println("[ACTION] No DuckyScript found");
            }
        }
        
        // UIManager will automatically return to Ready after 2s (handled in UIManager update loop)
        // No blocking delay needed - user can interact immediately when ready
        
    } else {
        char msg[80];
        if (benchElapsed > 0) {
            snprintf(msg, sizeof(msg), "Error (%.1fs)", benchElapsed / 1000.0f);
        } else {
            snprintf(msg, sizeof(msg), "Assistant Error");
        }
        Serial.printf("[AI] Query failed. Time=%u ms\n", benchElapsed);
        uiManager.postStatus(msg);
        uiManager.postStateChange(UIState::Error);
        ledRequest(LEDState::Error);
        
        delay(2000);
        uiManager.postStateChange(UIState::Ready);
    }
    
    g_bench_start_ms = 0;
    g_ai_task = nullptr;
    vTaskDelete(nullptr);
}

void setup() {
    Serial.begin(115200);  // UART0

    Serial.println("CuteAssistant starting...");

#ifdef USE_USB_KEYBOARD_ACTIONS
    ducky.begin();
    delay(100);
    Serial.println("[DUCKY] DuckyScript keyboard initialized (default: macOS)");
#endif

    // Initialize I2C bus FIRST (shared by display, touch, PMIC)
    Wire.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL);
    delay(100);
    
    // Initialize PMIC early to enable 5V bus for servos/peripherals
    initPMIC();

    // Initialize display and UI so user sees something immediately
    if (!display.init()) {
        Serial.println("Error: Display initialization failed");
        while (1) delay(1000);
    }

    // Initialize UI manager immediately after display
    if (!uiManager.init()) {
        Serial.println("Error: UI manager initialization failed");
        while (1) delay(1000);
    }
    
    // Show initial status immediately
    uiManager.postStateChange(UIState::Connecting);
    
    // Force display update so user sees the message
    updateDisplayNow();
    delay(100); // Brief pause to ensure rendering

    // Initialize LED manager
    LEDConfig ledConfig;
    ledConfig.pin = NEO_PIXEL_PIN;
    ledConfig.count = NEO_PIXEL_COUNT;
    ledConfig.brightness = 51; // 20% brightness
    
    if (!ledManager.init(ledConfig)) {
        Serial.println("Warning: LED manager initialization failed - continuing without LEDs");
        uiManager.postStatus("Starting...");
    } else {
        uiManager.postStatus("Starting...");
        ledManager.setState(LEDState::Processing); // Blue while system boots/connects
    }

    // Initialize audio manager
    updateDisplayNow();
    delay(50);
    
    AudioConfig audioConfig;
    audioConfig.sampleRate = RECORDING_SAMPLE_RATE;
    audioConfig.bitsPerSample = 16;
    audioConfig.numChannels = 1;
    audioConfig.maxRecordingMs = 30000; // 30 seconds max, but user can release earlier
    audioConfig.sckPin = MIC_I2S_SCK;
    audioConfig.wsPin = MIC_I2S_WS;
    audioConfig.doutPin = SPK_I2S_DOUT;
    audioConfig.dinPin = MIC_I2S_DIN;
    audioConfig.speakerEnableExpanderPin = EXPANDER_SPK_SHUTDOWN;
    audioConfig.speakerEnableExpanderAddr = IOEXP_I2C_ADDR;
    
    audioManager.setStateCallback(onAudioStateChanged);
    audioManager.setChunkCallback(onAudioChunkReady);
    if (!audioManager.init(audioConfig)) {
        Serial.println("Error: Audio manager initialization failed");
        uiManager.postStatus("Audio initialization failed");
        uiManager.postStateChange(UIState::Error);
        while (1) delay(1000);
    }

    // Create LED request queue
    g_led_queue = xQueueCreate(8, sizeof(uint8_t));
    
    // Create audio chunk streaming queue
    g_audio_chunk_queue = xQueueCreate(MAX_AUDIO_CHUNKS, sizeof(AudioChunk));
    if (!g_audio_chunk_queue) {
        Serial.println("[Setup] Error: Failed to create audio chunk queue");
    } else {
        Serial.println("[Setup] Audio streaming queue created successfully");
    }

    // Initialize top button (GPIO 0) as input with pull-up
    pinMode(BUTTON_TOP, INPUT_PULLUP);
    Serial.println("[Setup] Button TOP configured on GPIO 0");

    // NOW initialize WiFi and API credentials
    updateDisplayNow();
    delay(50);
    
    // Set up progress callback for WiFi connection
    wifiManager.setProgressCallback([](const char* message) {
        // Keep "Connecting..." state
    });
    
    bool wifiOk = wifiManager.loadCredentialsFromSD();
    bool apiOk = wifiManager.loadApiKeysFromSD();
    
    if (!wifiOk) {
        Serial.println("[WiFi] Could not load credentials from SD");
    }
    if (!apiOk) {
        Serial.println("[API] Could not load any API keys from SD (/apis.txt)");
    }

    // Determine preferred service based on preference string or availability
    if ((API_PREFER_STR.equalsIgnoreCase("deepseek") || API_PREFER_STR.equalsIgnoreCase("deepseek_force")) && DEEPSEEK_API_KEY_STR.length() > 0) {
        g_preferredService = AI_DEEPSEEK;
        g_forceDeepseek = API_PREFER_STR.equalsIgnoreCase("deepseek_force");
        Serial.printf("[Setup] Preferred AI service set to Deepseek%s\n", g_forceDeepseek ? " (force)" : "");
    } else if (API_PREFER_STR.equalsIgnoreCase("openai") && OPENAI_API_KEY_STR.length() > 0) {
        g_preferredService = AI_OPENAI;
        Serial.println("[Setup] Preferred AI service set to OpenAI");
    } else {
        // default to OpenAI if available
        if (OPENAI_API_KEY_STR.length() > 0) {
            g_preferredService = AI_OPENAI;
            Serial.println("[Setup] Defaulting preferred AI service to OpenAI");
        } else if (DEEPSEEK_API_KEY_STR.length() > 0) {
            g_preferredService = AI_DEEPSEEK;
            g_forceDeepseek = false;
            Serial.println("[Setup] Defaulting preferred AI service to Deepseek");
        } else {
            Serial.println("[Setup] No AI API key available; AI queries will fail");
        }
    }
    
    // Connect to WiFi
    updateDisplayNow();
    delay(100);
    
    wifiEnsureConnected();
    
    if (WiFi.status() == WL_CONNECTED) {
        uiManager.postStateChange(UIState::Ready);
        Serial.println("[Setup] CuteAssistant ready!");
    } else {
        uiManager.postStatus("WiFi failed");
        uiManager.postStateChange(UIState::Error);
    }
    
    if (g_preferredService == AI_OPENAI && OPENAI_API_KEY_STR.length() == 0) {
        Serial.println("[API] Warning: No OpenAI key found, queries will fail");
        uiManager.postStatus("No API key");
    } else if (g_preferredService == AI_DEEPSEEK && DEEPSEEK_API_KEY_STR.length() == 0) {
        Serial.println("[API] Warning: No Deepseek key found, queries will fail");
        uiManager.postStatus("No API key");
    }
}

void loop() {
    // Update all managers
    display.update();
    uiManager.update();
    audioManager.service();
    ledDrainRequests();
    
    // Update GPIO command sequence state machine
    updateCommandSequence();
    
    // Touch/Button handling for recording: press to start, release to stop
    static bool wasTouching = false;
    static bool wasButtonPressed = false;
    bool isTouching = false;
    bool isButtonPressed = false;
    
    // Check current touch state
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    if (indev) {
        lv_indev_state_t state = lv_indev_get_state(indev);
        isTouching = (state == LV_INDEV_STATE_PRESSED);
    }
    
    // Check top button state (active LOW with pull-up)
    isButtonPressed = (digitalRead(BUTTON_TOP) == LOW);
    
    // Combined input: either touch or button
    bool isInputActive = isTouching || isButtonPressed;
    bool wasInputActive = wasTouching || wasButtonPressed;
    
    // Detect input press edge (touch or button) - start recording
    if (isInputActive && !wasInputActive && 
        audioManager.getState() == RecordingState::Idle &&
        g_ai_task == nullptr &&
        millis() - g_last_touch_ms > TOUCH_DEBOUNCE_MS) {
        
        g_last_touch_ms = millis();
        
        // Clear chunk queue before starting
        AudioChunk chunk;
        while (xQueueReceive(g_audio_chunk_queue, &chunk, 0) == pdTRUE) {
            free(chunk.data);
        }
        
        // Enable streaming
        g_streaming_active = true;
        
        // Start recording
        uiManager.setResponse("");
        if (audioManager.startRecording()) {
            uiManager.postStatus("Listening...");
            if (isButtonPressed) {
                Serial.println("[Input] Recording started by TOP button - STREAMING ENABLED");
            } else {
                Serial.println("[Input] Recording started by touch - STREAMING ENABLED");
            }
        } else {
            uiManager.postStatus("Record failed");
            uiManager.postStateChange(UIState::Error);
            g_streaming_active = false;
        }
    }
    
    // Detect input release edge (touch or button) - stop recording
    if (!isInputActive && wasInputActive && 
        audioManager.getState() == RecordingState::Recording) {
        if (wasTouching) {
            Serial.println("[Input] Screen released");
        }
        audioManager.stopRecording();
        uiManager.postStatus("Processing...");
        Serial.println("[Input] Recording stopped - streaming will continue until task starts");
    }
    
    wasTouching = isTouching;
    wasButtonPressed = isButtonPressed;

    // Handle audio ready for AI processing (with safety delay)
    static uint32_t audioReadyTime = 0;
    if (audioManager.getState() == RecordingState::Saved && 
        g_ai_task == nullptr && 
        audioManager.getPCMBuffer() != nullptr && 
        audioManager.getPCMSize() > 0) {
        
        if (audioReadyTime == 0) {
            audioReadyTime = millis();
        } else if (millis() - audioReadyTime > 100) { // 100ms safety delay
            // Start AI query task and immediately update UI
            Serial.printf("[Main] Creating AI task for %zu bytes of audio\n", audioManager.getPCMSize());
            uiManager.postStateChange(UIState::Processing);
            
            BaseType_t result = xTaskCreatePinnedToCore(
                aiTask, 
                "ai_query", 
                12288, 
                nullptr, 
                1, 
                &g_ai_task, 
                1
            );
            
            if (result != pdPASS) {
                Serial.println("[Main] Failed to create OpenAI task");
                audioManager.resetToIdle();
                uiManager.postStatus("Failed to create OpenAI task");
                uiManager.postStateChange(UIState::Error);
                ledManager.setState(LEDState::Error);
            }
            audioReadyTime = 0;
        }
    } else if (audioManager.getState() != RecordingState::Saved) {
        audioReadyTime = 0;
    }

    // WiFi connectivity watchdog
    if (millis() - g_last_wifi_check_ms > WIFI_CHECK_INTERVAL_MS) {
        g_last_wifi_check_ms = millis();
        if (WiFi.status() != WL_CONNECTED) {
            uiManager.postStatus("Reconnecting WiFi...");
            wifiEnsureConnected();
        }
    }

    delay(GUI_LOOP_DELAY_MS);
}