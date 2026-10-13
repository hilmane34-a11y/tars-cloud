#include "vision_track.h"
#include <vision_live.h>
#include "stt.h"

#include <Arduino.h>
#include <WiFi.h>

extern bool playing;
extern volatile bool cameraTransition;
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

// Jangan mulai tracking ketika kamera atau STT sedang sibuk.
if (!visionLiveEnabled() ||
    playing ||
    cameraTransition ||
    WiFi.status() != WL_CONNECTED ||
    sttSessionActive) {
    return false;
}

// Ambil kepemilikan jalur tracking.
if (!visionLiveTrackPause()) {
    return false;
}

bool success = false;
size_t jpegLength = 0;

// Periksa ulang sebelum mengambil frame JPEG.
if (!cameraTransition &&
    !playing &&
    visionLiveEnabled() &&
    WiFi.status() == WL_CONNECTED &&
    !sttSessionActive) {

    if (visionLiveTrackCapture(jpegLength)) {
        uint8_t *jpeg = visionLiveSharedJpeg();

        // Pastikan data masih valid sebelum dikirim ke DETR.
        if (jpeg &&
            jpegLength > 0 &&
            !cameraTransition &&
            !playing &&
            visionLiveEnabled() &&
            WiFi.status() == WL_CONNECTED &&
            !sttSessionActive) {

            success = sttTrackDetect(
                jpeg,
                jpegLength,
                result,
                TRACK_TIMEOUT
            );
        }
    }
}

// Selalu lepaskan kepemilikan tracking setelah proses selesai.
visionLiveTrackResume();

// Abaikan hasil jika kamera sedang bertransisi atau mode berubah.
if (cameraTransition ||
    playing ||
    !visionLiveEnabled()) {

    result.found = false;
    result.score = 0.0f;
    result.centerX = 0.0f;
    result.targetX = -1;

    return false;
}

return success;

}
