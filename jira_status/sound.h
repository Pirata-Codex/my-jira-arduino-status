#pragma once
#include <Arduino.h>

// Room sound level from a MAX4466 microphone (analog, MIC_PIN). Measured continuously on core 0;
// the UI shows the stage in the header.
enum SoundStage : int8_t { SND_NONE = -1, SND_FOCUS, SND_NORMAL, SND_DISTRACTING, SND_TOO_LOUD };

void sound_begin();
SoundStage sound_stage();      // SND_NONE until the first seconds are measured (or no microphone)
float sound_db();              // smoothed level, approximate dB (see "Microphone calibration")
const char* sound_name(SoundStage s);
