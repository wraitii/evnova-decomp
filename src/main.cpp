#include <SDL3/SDL.h>

#include <cstdio>

int main(int, char**)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fputs(SDL_GetError(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }

    SDL_SetAppMetadata("Escape Velocity Nova", "0.1.0", "com.ambrosiasw.evnova");

    SDL_Window* window = SDL_CreateWindow("Escape Velocity Nova", 1280, 720, 0);
    if (window == nullptr) {
        std::fputs(SDL_GetError(), stderr);
        std::fputc('\n', stderr);
        SDL_Quit();
        return 1;
    }

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
