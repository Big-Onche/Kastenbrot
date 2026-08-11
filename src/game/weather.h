#ifndef __GAME_WEATHER_H__
#define __GAME_WEATHER_H__

namespace game
{
    namespace weather
    {
        void update(int seed);
        void reset();
        int getsettingsversion();
        float samplecoverage(float x, float y);
    }
}

#endif
