// Synthetic input for Nightcap, implemented by the SDL hooks. Events are
// queued and handed to Nightcap by its next event poll, so the game sees
// them exactly like the player's own. Main thread only.

#ifndef VOX_MENU_INJECT_H_
#define VOX_MENU_INJECT_H_

#include <SDL3/SDL.h>

#include <string>

namespace nac::inject {

// Positions are in window (logical) coordinates.
void MouseMove(float x, float y);
void MouseButton(int button, bool down, float x, float y);  // 0 left, 1 right, 2 middle
// `mod` is the modifiers the game sees held with it (Shift for a capital).
void Key(SDL_Scancode scancode, bool down, SDL_Keymod mod = SDL_KMOD_NONE);
void Text(const std::string& utf8);
// Moves the real cursor (it stays there).
void Warp(float x, float y);

}  // namespace nac::inject

#endif  // VOX_MENU_INJECT_H_
