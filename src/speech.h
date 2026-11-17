// Synthesizes chat messages and plays them into the mic and/or speakers.

#ifndef VOX_MENU_SPEECH_H_
#define VOX_MENU_SPEECH_H_

#include <SDL3/SDL.h>

#include <string>

namespace nac {

// Queue a message. force speaks even while disabled (the menu's test box).
void Say(const std::string& text, bool force = false);
void StopSpeaking();

// Called with every buffer Nightcap reads from a microphone stream, in that
// stream's output format, after the real read.
void MixIntoMic(void* data, int bytes, const SDL_AudioSpec& spec);

bool IsSpeaking();
// True while Nightcap is reading a microphone (you are in voice chat).
bool MicInUse();
std::string LastMessage();

}  // namespace nac

#endif  // VOX_MENU_SPEECH_H_
