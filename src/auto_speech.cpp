
#include "auto_speech.h"
#include "env.h"
#include "personality.h"

static AutoSpeechCallback speakCallback = nullptr;

static bool pending = false;
static bool eventPending = false;

static String eventPrompt;

static uint32_t lastAttempt = 0;
static uint32_t lastEvent = 0;

#define AUTO_SPEECH_COOLDOWN 120000
#define VISION_SPEECH_COOLDOWN 30000

void autoSpeechBegin(AutoSpeechCallback callback) {
  speakCallback = callback;

  pending = false;
  eventPending = false;

  eventPrompt = "";

  lastAttempt = 0;
  lastEvent = 0;
}

void autoSpeechNotifyVisionEvent(EnvEvent event) {
  if (pending || eventPending) return;

  if (millis() - lastEvent < VISION_SPEECH_COOLDOWN)
    return;

  switch (event) {
    case ENV_MOTION_LEFT:
      eventPrompt =
        "TARS mendeteksi perubahan gerakan di sebelah kiri. "
        "Buat pengumuman singkat dan natural kepada tuan. "
        "Jangan mengklaim bahwa itu manusia atau hewan.";
      break;

    case ENV_MOTION_CENTER:
      eventPrompt =
        "TARS mendeteksi sesuatu bergerak di depan. "
        "Buat pengumuman singkat dan natural kepada tuan. "
        "Jangan mengklaim identitas objek.";
      break;

    case ENV_MOTION_RIGHT:
      eventPrompt =
        "TARS mendeteksi perubahan gerakan di sebelah kanan. "
        "Buat pengumuman singkat kepada tuan. "
        "Jangan mengarang identitas objek.";
      break;

    case ENV_SCENE_CHANGED:
    case ENV_NONE:
    default:
      return;
  }

  eventPending = true;
  lastEvent = millis();
}

void autoSpeechUpdate(
  bool enabled,
  bool listening,
  bool speaking
) {
  if (!enabled || listening || speaking ||
      pending || !speakCallback)
    return;

  if (eventPending) {
    pending = true;
    eventPending = false;

    if (!speakCallback(eventPrompt))
      pending = false;

    eventPrompt = "";
    return;
  }

  if (millis() - lastAttempt < AUTO_SPEECH_COOLDOWN)
    return;

  if (!personalityWantsSpeak())
    return;

  lastAttempt = millis();
  pending = true;

  if (!speakCallback(
    "Kamu adalah TARS, robot AI buatan Ilman. "
    "Buat ucapan spontan singkat dalam bahasa Indonesia "
    "kepada tuan. Natural, sedikit penasaran, maksimal 20 kata."
  )) {
    pending = false;
  }
}

void autoSpeechDone() {
  if (!pending) return;

  pending = false;
  personalitySpeechDone();
}
