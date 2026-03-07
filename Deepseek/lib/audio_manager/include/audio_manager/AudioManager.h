#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include <Arduino.h>
#include <ESP_I2S.h>
#include <TCA9555.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <functional>

enum class RecordingState { 
    Idle, 
    Recording, 
    Saving, 
    Saved, 
    Error 
};

struct AudioConfig {
    uint32_t sampleRate = 16000;
    uint32_t playbackSampleRate = 16000;
    uint16_t bitsPerSample = 16;
    uint16_t numChannels = 1;
    uint32_t maxRecordingMs = 5000;
    uint32_t speakerPowerupDelayMs = 20;
    uint32_t playbackTailMs = 40;
    int8_t sckPin = -1;
    int8_t wsPin = -1;
    int8_t doutPin = -1;
    int8_t dinPin = -1;
    int8_t speakerEnableExpanderPin = -1;
    uint8_t speakerEnableExpanderAddr = 0x20;
    // Keep amp enabled after init to avoid frequent I2C toggles during playback.
    bool keepSpeakerEnabled = true;
};

typedef std::function<void(RecordingState)> StateCallback;
typedef std::function<void(const uint8_t*, size_t)> ChunkCallback;

class AudioManager {
public:
    AudioManager();
    ~AudioManager();
    
    bool init(const AudioConfig& config);
    void setStateCallback(StateCallback callback);
    void setChunkCallback(ChunkCallback callback);
    
    // Recording control
    bool startRecording();
    void stopRecording();
    RecordingState getState() const { return state_; }
    
    // Audio data access
    uint8_t* getPCMBuffer() const { return pcmBuffer_; }
    size_t getPCMSize() const { return pcmWritten_; }
    void releasePCMBuffer();
    void resetToIdle(); // Reset state to idle after processing
    
    // Audio playback (for Realtime API)
    bool playPCM16(const uint8_t* data, size_t len);
    
    // Processing
    static void normalizeAndLimitPCM16(uint8_t* data, size_t numBytes, 
                                     float targetPeakFS = 0.89f, 
                                     float kneeFS = 0.707f, 
                                     float ratio = 3.0f);
    
    // Service function - call from main loop
    void service();
    
private:
    AudioConfig config_;
    I2SClass i2s_;
    RecordingState state_;
    StateCallback stateCallback_;
    ChunkCallback chunkCallback_;
    
    // Recording data
    uint8_t* pcmBuffer_;
    size_t pcmCapacity_;
    size_t pcmWritten_;
    uint32_t recordStartMs_;
    
    // Task management
    TaskHandle_t captureTask_;
    
    // Internal methods
    void setState(RecordingState newState);
    static void captureTaskImpl(void* param);
    void captureLoop();
    bool allocatePCMBuffer();
    void freePCMBuffer();
    bool configureInputI2S();
    bool configureOutputI2S();
    bool initSpeakerExpander();
    void setSpeakerEnabled(bool enabled);

    TCA9555* speakerExpander_;
    bool speakerExpanderReady_;
};

#endif // AUDIO_MANAGER_H
