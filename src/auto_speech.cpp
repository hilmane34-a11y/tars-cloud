
#include "auto_speech.h"
#include "env.h"
#include "personality.h"

static AutoSpeechCallback speakCallback = nullptr;
static bool pending = false;
static String visionPrompt;
static String lastDescription;

static uint32_t nextSpeechTime = 0;

#define AUTO_SPEECH_MIN 180000UL
#define AUTO_SPEECH_MAX 420000UL

static void scheduleNext() {
  nextSpeechTime = millis() +
    random(AUTO_SPEECH_MIN, AUTO_SPEECH_MAX + 1);
}

void autoSpeechBegin(AutoSpeechCallback callback) {
  speakCallback = callback;
  pending = false;
  visionPrompt = "";
  lastDescription = "";
  nextSpeechTime = millis() + AUTO_SPEECH_MIN;
}

void autoSpeechNotifyVision(const String &description) {
  if (pending || !description.length()) return;
  if (description == lastDescription) return;

  visionPrompt =
    "Kamu adalah TARS, robot AI buatan Ilman. "
    "Berikut hasil pengamatan visual yang benar-benar terdeteksi: " +
    description +
    ". Sampaikan pengamatan tersebut secara singkat, sopan, "
    "dan natural kepada tuan. Jangan mengarang objek, "
    "identitas, warna, atau kejadian. Maksimal 20 kata. "
    "Jika informasinya tidak cukup, jangan berbicara.";

  lastDescription = description;
}

void autoSpeechNotifyVisionEvent(EnvEvent event) {
  switch (event) {
    case ENV_MOTION_LEFT:
      autoSpeechNotifyVision(
        "Terlihat perubahan gerakan di sebelah kiri kamera."
      );
      break;

    case ENV_MOTION_CENTER:
      autoSpeechNotifyVision(
        "Terlihat perubahan gerakan di depan kamera."
      );
      break;

    case ENV_MOTION_RIGHT:
      autoSpeechNotifyVision(
        "Terlihat perubahan gerakan di sebelah kanan kamera."
      );
      break;

    default:
      break;
  }
}

void autoSpeechUpdate(
  bool enabled,
  bool listening,
  bool speaking
) {
  if (!enabled || listening || speaking ||
      pending || !speakCallback)
    return;

  if ((int32_t)(millis() - nextSpeechTime) < 0)
    return;

  if (!visionPrompt.length())
    return;

  pending = true;

  if (speakCallback(visionPrompt)) {
    visionPrompt = "";
    scheduleNext();
  } else {
    pending = false;
  }
}

void autoSpeechDone() {
  if (!pending) return;

  pending = false;
  personalitySpeechDone();
}
