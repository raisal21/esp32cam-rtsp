#pragma once

#include <Arduino.h>
#include "FS.h"
#include "SPIFFS.h"
#include <WiFi.h>
#include <esp_camera.h>

class VideoFrameProvider {
private:
    // Storage for video frames
    uint8_t* frameBuffer;   
    size_t frameBufferSize;
    
    // Frame metadata
    uint32_t numFrames;
    uint32_t currentFrameIndex;
    uint32_t* frameSizes;
    uint32_t* frameOffsets;
    
    // Frame rate control
    unsigned long lastFrameTime;
    unsigned long frameInterval; // in milliseconds

public:
    VideoFrameProvider() : 
        frameBuffer(nullptr), 
        numFrames(0), 
        currentFrameIndex(0),
        frameSizes(nullptr),
        frameOffsets(nullptr),
        lastFrameTime(0),
        frameInterval(100) // Default 10 FPS
    {}

    ~VideoFrameProvider() {
        if (frameBuffer) {
            free(frameBuffer);
        }
        if (frameSizes) {
            free(frameSizes);
        }
        if (frameOffsets) {
            free(frameOffsets);
        }
    }

    bool init(const char* videoFilePath, unsigned long interval) {
        log_i("Initializing VideoFrameProvider with file: %s", videoFilePath);
        
        // Set frame interval
        frameInterval = interval;
        
        // Initialize SPIFFS
        if (!SPIFFS.begin(true)) {
            log_e("Failed to initialize SPIFFS");
            return false;
        }
        
        // Open video metadata file
        File metadataFile = SPIFFS.open("/video_metadata.bin", "r");
        if (!metadataFile) {
            log_e("Failed to open metadata file");
            return false;
        }
        
        // Read number of frames
        metadataFile.read((uint8_t*)&numFrames, sizeof(numFrames));
        log_i("Number of frames: %d", numFrames);
        
        // Allocate memory for frame sizes and offsets
        frameSizes = (uint32_t*)malloc(numFrames * sizeof(uint32_t));
        frameOffsets = (uint32_t*)malloc(numFrames * sizeof(uint32_t));
        
        if (!frameSizes || !frameOffsets) {
            log_e("Failed to allocate memory for frame metadata");
            if (frameSizes) free(frameSizes);
            if (frameOffsets) free(frameOffsets);
            metadataFile.close();
            return false;
        }
        
        // Read frame sizes and calculate offsets
        uint32_t offset = 0;
        for (uint32_t i = 0; i < numFrames; i++) {
            metadataFile.read((uint8_t*)&frameSizes[i], sizeof(uint32_t));
            frameOffsets[i] = offset;
            offset += frameSizes[i];
        }
        
        metadataFile.close();
        
        // Open video frames file
        File framesFile = SPIFFS.open(videoFilePath, "r");
        if (!framesFile) {
            log_e("Failed to open frames file");
            free(frameSizes);
            free(frameOffsets);
            return false;
        }
        
        // Get total size of all frames
        frameBufferSize = framesFile.size();
        log_i("Total frame buffer size: %d bytes", frameBufferSize);
        
        // Allocate memory for frames
        frameBuffer = (uint8_t*)malloc(frameBufferSize);
        if (!frameBuffer) {
            log_e("Failed to allocate memory for frame buffer");
            free(frameSizes);
            free(frameOffsets);
            framesFile.close();
            return false;
        }
        
        // Read all frames at once
        if (framesFile.read(frameBuffer, frameBufferSize) != frameBufferSize) {
            log_e("Failed to read frames");
            free(frameBuffer);
            free(frameSizes);
            free(frameOffsets);
            framesFile.close();
            return false;
        }
        
        framesFile.close();
        log_i("VideoFrameProvider initialization complete");
        return true;
    }

    // Get the next frame
    camera_fb_t* getFrame() {
        // Check if it's time for a new frame based on frame rate
        unsigned long currentTime = millis();
        if (currentTime - lastFrameTime < frameInterval) {
            return nullptr; // Not time for a new frame yet
        }
        
        lastFrameTime = currentTime;
        
        // Create a camera_fb_t structure to mimic the camera interface
        camera_fb_t* fb = (camera_fb_t*)malloc(sizeof(camera_fb_t));
        if (!fb) {
            log_e("Failed to allocate memory for frame buffer structure");
            return nullptr;
        }
        
        // Fill the structure with the current frame data
        fb->buf = frameBuffer + frameOffsets[currentFrameIndex];
        fb->len = frameSizes[currentFrameIndex];
        fb->width = 640;  // Assuming fixed resolution for now
        fb->height = 480; // Assuming fixed resolution for now
        fb->format = PIXFORMAT_JPEG;
        fb->timestamp.tv_sec = currentTime / 1000;
        fb->timestamp.tv_usec = (currentTime % 1000) * 1000;
        
        // Move to the next frame
        currentFrameIndex = (currentFrameIndex + 1) % numFrames;
        
        return fb;
    }

    // Release a frame (to mimic esp_camera_fb_return)
    void returnFrame(camera_fb_t* fb) {
        if (fb) {
            free(fb);
        }
    }

    // Utility to get current frames per second
    float getCurrentFps() {
        return 1000.0f / frameInterval;
    }
    
    // Set frames per second
    void setFps(float fps) {
        if (fps <= 0) fps = 10.0f; // Fallback to 10 FPS
        frameInterval = 1000.0f / fps;
    }
};