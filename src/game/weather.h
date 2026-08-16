#ifndef __GAME_WEATHER_H__
#define __GAME_WEATHER_H__

namespace game
{
    namespace weather
    {
        void update(int seed);
        bool preparemap(const char *folder, int seed);
        void reset();
        void clearsync();
        void synctime(int seed, uint millis, float cloudspeed, float windangle);
        int getseed(int fallback);
        double gettimemillis();
        float getcloudspeed(float fallback);
        float getwindangle(float fallback);
        int getsettingsversion();
        int getcoverageversion();
        float samplecoverage(float x, float y);
        float samplecurrentovercast(float x, float y);
        void addparticles();
    }
}

#endif
