#include "auto_speech.h"
#include "personality.h"

static AutoSpeechCallback speakCallback = nullptr;

static bool pending = false;
static bool processing = false;

static String randomBoredSpeech()
{
    switch (random(6))
    {
        case 0:
            return "[AUTO_CHAT] Haa... bosan sekali.";

        case 1:
            return "[AUTO_CHAT] Hmm... sepi sekali.";

        case 2:
            return "[AUTO_CHAT] Haa... lama sekali.";

        case 3:
            return "[AUTO_CHAT] Aku mulai bosan nih.";

        case 4:
            return "[AUTO_CHAT] Hmm... tidak ada kegiatan.";

        default:
            return "[AUTO_CHAT] Haa... ingin melakukan sesuatu.";
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
    // Vision tidak pernah memicu auto-speech.
    (void)description;
}

void autoSpeechNotifyVisionEvent(EnvEvent event)
{
    // Vision event tidak pernah memicu auto-speech.
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

    if (personalityIsResting())
        return;

    PersonalityState state = personalityGet();

    if (state.energy <= 20 ||
        state.fatigue >= 80)
        return;

    // Hanya personality boredom yang boleh memicu.
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

    // Cooldown 10 menit dimulai setelah TTS selesai.
    personalitySpeechDone();
}

void autoSpeechResetTimer()
{
    pending = false;
    processing = false;
}
