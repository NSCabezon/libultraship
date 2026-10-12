// SDL_IsIPad() lives in SDL's UIKit video backend, which is compiled out on
// visionOS (SDL_VIDEO=OFF), but SDL.c's SDL_IsTablet() still references it.
#include "SDL_stdinc.h"

SDL_bool SDL_IsIPad(void) {
    return SDL_FALSE;
}
