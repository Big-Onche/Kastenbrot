#ifndef __GAME_WEATHER_H__
#define __GAME_WEATHER_H__

namespace game
{
    namespace weather
    {
        void update(int seed);
        void reset();
        void clearsync();
        void synctime(int seed, uint millis, int updateinterval, float weatherspeed, float cloudspeed, float windangle);
        int getseed(int fallback);
        double gettimemillis();
        int getupdateinterval(int fallback);
        float getweatherspeed(float fallback);
        float getcloudspeed(float fallback);
        float getwindangle(float fallback);
        int getsettingsversion();
        float samplecoverage(float x, float y);
        float samplecurrentovercast(float x, float y);
        void addparticles();
    }
}

#endif
