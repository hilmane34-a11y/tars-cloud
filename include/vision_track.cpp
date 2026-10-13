#include "vision_track.h"
#include "vision_live.h"
#include "stt.h"

#include <Arduino.h>
#include <WiFi.h>

extern bool playing;
extern bool visionLiveEnabled();

#define TRACK_TIMEOUT 8000

void visionTrackBegin() {
    Serial.println("TARS TRACK: SHARED STT WSS READY");
}

bool visionTrackPerson(VisionTrackResult &result) {
    result.found = false;
    result.score = 0.0f;
    result.centerX = 0.0f;
    result.targetX = -1;

    if (!visionLiveEnabled() ||
        playing ||
        WiFi.status() != WL_CONNECTED ||
        sttSessionActive) {
        return false;
    }

    if (!visionLiveTrackPause()) {
        return false;
    }

    bool success = false;
    size_t jpegLength = 0;

    if (visionLiveTrackCapture(jpegLength)) {
        uint8_t *jpeg = visionLiveSharedJpeg();

        if (jpeg && jpegLength > 0) {
            success = sttTrackDetect(
                jpeg,
                jpegLength,
                result,
                TRACK_TIMEOUT
            );
        }
    }

    // Lepaskan buffer kamera setelah permintaan selesai.
    visionLiveTrackResume();

    return success;
}
