#include "audio_manager/AudioManager.h"
#include <esp_heap_caps.h>
#include <math.h>
#include <cstring>

namespace {

constexpr size_t kPlaybackChunkBytes = 2048;

}

AudioManager::AudioManager() 
    : state_(RecordingState::Idle)
    , stateCallback_(nullptr)
    , chunkCallback_(nullptr)
    , pcmBuffer_(nullptr)
    , pcmCapacity_(0)
    , pcmWritten_(0)
    , recordStartMs_(0)
    , captureTask_(nullptr)
    , speakerExpander_(nullptr)
    , speakerExpanderReady_(false) {
}

AudioManager::~AudioManager() {
    if (captureTask_) {
        vTaskDelete(captureTask_);
    }
    setSpeakerEnabled(false);
    if (speakerExpander_) {
        delete speakerExpander_;
        speakerExpander_ = nullptr;
    }
    freePCMBuffer();
}

bool AudioManager::init(const AudioConfig& config) {
    config_ = config;

    if (!initSpeakerExpander()) {
        Serial.println("[AudioManager] Speaker expander init failed");
    }

    if (!configureInputI2S()) {
        Serial.println("[AudioManager] I2S input init failed");
        setState(RecordingState::Error);
        return false;
    }

    setState(RecordingState::Idle);
    return true;
}

void AudioManager::setStateCallback(StateCallback callback) {
    stateCallback_ = callback;
}

void AudioManager::setChunkCallback(ChunkCallback callback) {
    chunkCallback_ = callback;
}

bool AudioManager::startRecording() {
    if (state_ != RecordingState::Idle && state_ != RecordingState::Saved) {
        return false;
    }
    
    // Allocate PCM buffer
    if (!allocatePCMBuffer()) {
        setState(RecordingState::Error);
        return false;
    }
    
    recordStartMs_ = millis();
    setState(RecordingState::Recording);
    
    // Create capture task
    BaseType_t result = xTaskCreatePinnedToCore(
        captureTaskImpl, 
        "audio_capture", 
        4096, 
        this, 
        2, 
        &captureTask_, 
        0
    );
    
    if (result != pdPASS) {
        Serial.println("[AudioManager] Failed to create capture task");
        setState(RecordingState::Error);
        return false;
    }
    
    return true;
}

void AudioManager::stopRecording() {
    if (state_ == RecordingState::Recording) {
        setState(RecordingState::Saving);
    }
}

void AudioManager::service() {
    if (state_ != RecordingState::Recording) {
        return;
    }
    
    // Check for timeout
    uint32_t elapsed = millis() - recordStartMs_;
    if (elapsed >= config_.maxRecordingMs) {
        stopRecording();
    }
}

void AudioManager::setState(RecordingState newState) {
    if (state_ != newState) {
        state_ = newState;
        if (stateCallback_) {
            stateCallback_(state_);
        }
    }
}

void AudioManager::captureTaskImpl(void* param) {
    AudioManager* self = static_cast<AudioManager*>(param);
    self->captureLoop();
    self->captureTask_ = nullptr;
    vTaskDelete(nullptr);
}

void AudioManager::captureLoop() {
    while (state_ == RecordingState::Recording) {
        size_t wavSize = 0;
        uint8_t* wav = i2s_.recordWAV(1, &wavSize);
        
        if (wav && wavSize > 44 && pcmBuffer_) {
            size_t payload = wavSize - 44; // Skip WAV header
            size_t space = (pcmCapacity_ > pcmWritten_) ? (pcmCapacity_ - pcmWritten_) : 0;
            size_t toCopy = (payload < space) ? payload : space;
            
            if (toCopy > 0) {
                memcpy(pcmBuffer_ + pcmWritten_, wav + 44, toCopy);
                pcmWritten_ += toCopy;
                
                // Enviar chunk mientras se graba
                if (chunkCallback_) {
                    chunkCallback_(wav + 44, toCopy);
                }
            }
            
            // Check if buffer is full
            if (pcmWritten_ >= pcmCapacity_ && state_ == RecordingState::Recording) {
                setState(RecordingState::Saving);
            }
        } else if (!wav) {
            Serial.println("[AudioManager] recordWAV returned NULL");
        } else if (wavSize <= 44) {
            Serial.println("[AudioManager] WAV too small");
        }
        
        if (wav) {
            free(wav);
        }
    }
    
    Serial.println("[AudioManager] Capture loop ended, processing audio...");
    
    // Wait a bit to ensure all threads are synchronized
    delay(10);
    
    // Processing: normalize to -1 dBFS and softly limit to -3 dBFS (ratio 3:1)
    if (pcmBuffer_ && pcmWritten_ > 0) {
        Serial.printf("[AudioManager] Normalizing %zu bytes of PCM data\n", pcmWritten_);
        normalizeAndLimitPCM16(pcmBuffer_, pcmWritten_);
        Serial.println("[AudioManager] Audio processing completed");
    } else {
        Serial.println("[AudioManager] No valid audio data to process");
    }
    
    setState(RecordingState::Saved);
}

bool AudioManager::allocatePCMBuffer() {
    freePCMBuffer(); // Free any existing buffer
    
    uint32_t bytesPerSec = config_.sampleRate * config_.numChannels * (config_.bitsPerSample / 8);
    pcmCapacity_ = bytesPerSec * 10; // 10 seconds max
    pcmWritten_ = 0;
    
    // Try PSRAM first, then regular RAM
    pcmBuffer_ = (uint8_t*)heap_caps_malloc(pcmCapacity_, MALLOC_CAP_SPIRAM);
    if (!pcmBuffer_) {
        pcmBuffer_ = (uint8_t*)malloc(pcmCapacity_);
    }
    
    if (!pcmBuffer_) {
        Serial.println("[AudioManager] Failed to allocate PCM buffer");
        return false;
    }
    
    return true;
}

void AudioManager::freePCMBuffer() {
    if (pcmBuffer_) {
        Serial.printf("[AudioManager] Freeing PCM buffer (%zu bytes)\n", pcmCapacity_);
        free(pcmBuffer_);
        pcmBuffer_ = nullptr;
    }
    pcmCapacity_ = 0;
    pcmWritten_ = 0;
}

void AudioManager::releasePCMBuffer() {
    // Thread-safe buffer release with additional protection
    if (captureTask_) {
        Serial.println("[AudioManager] WARNING: Releasing buffer while capture task is active");
        // Force stop capture task if still running
        vTaskDelete(captureTask_);
        captureTask_ = nullptr;
        delay(10); // Give time for cleanup
    }
    
    freePCMBuffer();
}

void AudioManager::resetToIdle() {
    Serial.println("[AudioManager] Resetting to Idle state");
    setState(RecordingState::Idle);
}

void AudioManager::normalizeAndLimitPCM16(uint8_t* data, size_t numBytes, 
                                        float targetPeakFS, float kneeFS, float ratio) {
    if (!data || numBytes < 2) return;
    
    int16_t* samples = (int16_t*)data;
    size_t numSamples = numBytes / 2;
    
    // Find peak
    int32_t maxAbs = 0;
    for (size_t i = 0; i < numSamples; ++i) {
        int32_t v = samples[i];
        int32_t a = v < 0 ? -v : v;
        if (a > maxAbs) maxAbs = a;
    }
    
    if (maxAbs <= 0) return;
    
    const float fs = 32767.0f;
    const float targetAbs = targetPeakFS * fs;
    float gain = targetAbs / (float)maxAbs;
    const float kneeAbs = kneeFS * fs;
    
    // Apply gain and soft limiting
    for (size_t i = 0; i < numSamples; ++i) {
        float x = (float)samples[i] * gain;
        float sgn = (x >= 0.0f) ? 1.0f : -1.0f;
        float ax = fabsf(x);
        
        if (ax > kneeAbs) {
            float over = ax - kneeAbs;
            ax = kneeAbs + over / ratio;
        }
        
        if (ax > fs) ax = fs;
        float y = sgn * ax;
        
        if (y > 32767.0f) y = 32767.0f;
        if (y < -32768.0f) y = -32768.0f;
        
        samples[i] = (int16_t)lroundf(y);
    }
}

bool AudioManager::playPCM16(const uint8_t* data, size_t len) {
    if (!data || len == 0) return false;

    if (state_ == RecordingState::Recording || captureTask_) {
        Serial.println("[AudioManager] Rejecting playback while recording is active");
        return false;
    }

    if (config_.doutPin < 0) {
        Serial.println("[AudioManager] Playback requested without a DOUT pin configured");
        return false;
    }

    Serial.printf("[AudioManager] Starting playback: %u bytes\n", (unsigned)len);
    if (!configureOutputI2S()) {
        Serial.println("[AudioManager] Failed to switch I2S to TX mode");
        configureInputI2S();
        return false;
    }

    setSpeakerEnabled(true);
    if (config_.speakerPowerupDelayMs > 0) {
        delay(config_.speakerPowerupDelayMs);
    }

    bool success = true;
    size_t totalWritten = 0;
    while (totalWritten < len) {
        const size_t remaining = len - totalWritten;
        const size_t chunkSize = remaining > kPlaybackChunkBytes ? kPlaybackChunkBytes : remaining;
        const size_t written = i2s_.write(data + totalWritten, chunkSize);
        if (written == 0) {
            Serial.printf("[AudioManager] Playback stalled after %u bytes\n", (unsigned)totalWritten);
            success = false;
            break;
        }
        totalWritten += written;
    }

    if (config_.playbackTailMs > 0) {
        delay(config_.playbackTailMs);
    }
    setSpeakerEnabled(false);

    if (!configureInputI2S()) {
        Serial.println("[AudioManager] WARNING: Failed to restore I2S RX mode after playback");
        setState(RecordingState::Error);
        success = false;
    }

    Serial.printf("[AudioManager] Playback %s (%u/%u bytes)\n",
                  success ? "finished" : "failed",
                  (unsigned)totalWritten,
                  (unsigned)len);
    return success && totalWritten == len;
}

bool AudioManager::configureInputI2S() {
    i2s_.end();
    i2s_.setPins(config_.sckPin, config_.wsPin, -1, config_.dinPin);
    Serial.printf("[AudioManager] Configuring I2S RX (%u Hz, DIN=%d)\n",
                  config_.sampleRate,
                  config_.dinPin);
    return i2s_.begin(I2S_MODE_STD,
                      config_.sampleRate,
                      I2S_DATA_BIT_WIDTH_16BIT,
                      I2S_SLOT_MODE_MONO,
                      I2S_STD_SLOT_LEFT);
}

bool AudioManager::configureOutputI2S() {
    i2s_.end();
    i2s_.setPins(config_.sckPin, config_.wsPin, config_.doutPin, -1);
    Serial.printf("[AudioManager] Configuring I2S TX (%u Hz, DOUT=%d)\n",
                  config_.playbackSampleRate,
                  config_.doutPin);
    return i2s_.begin(I2S_MODE_STD,
                      config_.playbackSampleRate,
                      I2S_DATA_BIT_WIDTH_16BIT,
                      I2S_SLOT_MODE_MONO,
                      I2S_STD_SLOT_LEFT);
}

bool AudioManager::initSpeakerExpander() {
    speakerExpanderReady_ = false;

    if (config_.speakerEnableExpanderPin < 0) {
        return true;
    }

    if (speakerExpander_) {
        delete speakerExpander_;
        speakerExpander_ = nullptr;
    }

    speakerExpander_ = new TCA9555(config_.speakerEnableExpanderAddr, &Wire);
    if (!speakerExpander_) {
        Serial.println("[AudioManager] Failed to allocate speaker expander instance");
        return false;
    }

    if (!speakerExpander_->begin()) {
        Serial.println("[AudioManager] Speaker expander not responding");
        return false;
    }
    if (!speakerExpander_->pinMode1(config_.speakerEnableExpanderPin, OUTPUT)) {
        Serial.println("[AudioManager] Failed to configure speaker enable pin");
        return false;
    }

    speakerExpanderReady_ = true;
    setSpeakerEnabled(false);
    return true;
}

void AudioManager::setSpeakerEnabled(bool enabled) {
    if (config_.speakerEnableExpanderPin < 0) {
        return;
    }
    if (!speakerExpanderReady_ || !speakerExpander_) {
        Serial.printf("[AudioManager] Speaker expander unavailable, cannot set enable=%d\n", enabled ? 1 : 0);
        return;
    }

    if (!speakerExpander_->write1(config_.speakerEnableExpanderPin, enabled ? HIGH : LOW)) {
        Serial.printf("[AudioManager] Failed to set speaker enable=%d\n", enabled ? 1 : 0);
    }
}
