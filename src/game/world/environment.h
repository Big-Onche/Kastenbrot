#ifndef __GAME_WORLD_ENVIRONMENT_H__
#define __GAME_WORLD_ENVIRONMENT_H__

#ifndef STANDALONE
namespace game
{
    namespace environment
    {
        extern void reset();
        extern void update();
        extern void synctime(int millis, bool frozen);
        extern int gettimemillis();
        extern float getdayprogress();
        extern float gethourafter(int millis);
        extern bool istimefrozen();
        extern float getambientlightlevel();
    }
}
#endif

#endif
