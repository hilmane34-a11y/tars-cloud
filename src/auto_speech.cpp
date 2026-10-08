#include "auto_speech.h"
#include "personality.h"
#include <tars_emotion.h>

static AutoSpeechCallback speakCallback = nullptr;

static bool pending = false;
static bool processing = false;

#define AUTO_SPEECH_COOLDOWN 600000UL   // 10 menit

static String randomBoredSpeech()
{
    switch (random(5))
    {
        case 0:
            return "[AUTO_CHAT] Haa... sudah terlalu lama. Bosan sekali.";

        case 1:
            return "[AUTO_CHAT] Tuan... aku mulai bosan.";

        case 2:
            return "[AUTO_CHAT] Hmm... sepi sekali.";

        case 3:
            return "[AUTO_CHAT] Aku bosan nih, tuan.";

        default:
            return "[AUTO_CHAT] Haa... lama sekali tidak diajak bicara.";
    }
}

void autoSpeechBegin(AutoSpeechCallback callback)
{
    speakCallback = callback;

    pending = false;
    processing = false;

    randomSeed(micros());
}

void autoSpeechNotifyVision(const String &description)
{
    // Vision TIDAK BOLEH MEMICU AUTO SPEECH.
    // Data vision tetap boleh dipakai oleh sistem lain.
    (void)description;
}

void autoSpeechNotifyVisionEvent(EnvEvent event)
{
    // Vision event TIDAK BOLEH MEMICU AUTO SPEECH.
    (void)event;
}

void autoSpeechUpdate(
    bool enabled,
    bool listening,
    bool speaking
)
{
    if (!enabled ||
        listening ||
        speaking ||
        pending ||
        processing ||
        !speakCallback)
        return;

    if (tarsEmotionHasEvent())
        return;

    if (personalityIsResting())
        return;

    PersonalityState state = personalityGet();

    if (state.energy <= 20 ||
        state.fatigue >= 80)
        return;

    // HANYA BOLEH KELUAR KARENA BOSAN
    if (!personalityWantsSpeak())
        return;

    String prompt = randomBoredSpeech();

    pending = true;
    processing = true;

    bool accepted = speakCallback(prompt);

    if (!accepted)
    {
        pending = false;
        processing = false;
    }
}

void autoSpeechDone()
{
    if (!pending)
        return;

    pending = false;
    processing = false;

    // Timer cooldown dimulai setelah TTS berhasil selesai.
    personalitySpeechDone();
}

void autoSpeechResetTimer()
{
    pending = false;
    processing = false;
}
