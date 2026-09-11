#ifndef __GAME_GRASSCOLOR_H__
#define __GAME_GRASSCOLOR_H__

struct vec;
struct worldgencontext;

namespace game
{
    // Main-thread rendering query, in absolute engine coordinates. RGB only; never modifies asset alpha.
    extern vec getgrassworldcolor(const vec &absolute);
    // Worker-local climate query; never accesses the main-thread environment cache.
    extern vec samplegrassgenerationcolor(worldgencontext *generation, const vec &absolute);
}

#endif
