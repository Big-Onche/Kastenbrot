#include "game.h"
#include "weather.h"

#ifdef SQRT3
#pragma push_macro("SQRT3")
#undef SQRT3
#define RESTORE_WEATHER_SQRT3
#endif
#include "FastNoiseLite.h"
#ifdef RESTORE_WEATHER_SQRT3
#pragma pop_macro("SQRT3")
#undef RESTORE_WEATHER_SQRT3
#endif

// With the default cloud shape scale, weather systems are roughly thirty times
// wider than a typical cloud feature.
FVARP(weatherscale, 0.000001f, 0.00003f, 0.001f);
FVARP(weatherwarpscale, 0.000001f, 0.000015f, 0.001f);
FVARP(weatherwarpamplitude, 0.0f, 6000.0f, 50000.0f);

// These normalized noise boundaries give about 20% clear, 55% fair, and 25%
// overcast weather regions, favoring fair cumulus conditions.
FVARP(weatherclearthreshold, 0.0f, 0.35f, 1.0f);
FVARP(weatherfairthreshold, 0.0f, 0.50f, 1.0f);
FVARP(weatherovercastthreshold, 0.0f, 0.625f, 1.0f);
FVARP(weathertransitionwidth, 0.001f, 0.06f, 0.5f);

// Coverage is the desired fraction of the local cloud-shape mask, not another density signal.
FVARP(weatherfairmincoverage, 0.0f, 0.22f, 1.0f);
FVARP(weatherfairmaxcoverage, 0.0f, 0.55f, 1.0f);
FVARP(weatherovercastcoverage, 0.0f, 0.95f, 1.0f);

extern int cloudupdateinterval;
extern float weatherwindspeed, cloudwindangle;

namespace game
{
    extern int getworldseed();

    namespace weather
    {
        namespace
        {
            static FastNoiseLite weathernoise, weatherwarp;
            static int noiseseed = INT_MIN, settingsversion = 0;
            static uint currentsettingshash = 0;

            static uint mixhash(uint value)
            {
                value ^= value >> 16;
                value *= 0x7FEB352DU;
                value ^= value >> 15;
                value *= 0x846CA68BU;
                value ^= value >> 16;
                return value;
            }

            static uint hashfloat(float value)
            {
                union
                {
                    float f;
                    uint i;
                } bits;
                bits.f = value;
                return bits.i;
            }

            static void addhash(uint &hash, uint value)
            {
                hash = mixhash(hash ^ value);
            }

            static uint settingshash(int seed)
            {
                uint hash = mixhash(uint(seed));
                addhash(hash, hashfloat(weatherscale));
                addhash(hash, hashfloat(weatherwarpscale));
                addhash(hash, hashfloat(weatherwarpamplitude));
                addhash(hash, hashfloat(weatherclearthreshold));
                addhash(hash, hashfloat(weatherfairthreshold));
                addhash(hash, hashfloat(weatherovercastthreshold));
                addhash(hash, hashfloat(weathertransitionwidth));
                addhash(hash, hashfloat(weatherfairmincoverage));
                addhash(hash, hashfloat(weatherfairmaxcoverage));
                addhash(hash, hashfloat(weatherovercastcoverage));
                return hash;
            }

            static float smoothstep(float low, float high, float value)
            {
                if(high <= low) return value >= high ? 1.0f : 0.0f;
                const float t = clamp((value - low) / (high - low), 0.0f, 1.0f);
                return t * t * (3.0f - 2.0f * t);
            }

            static float fairprogress(float value, float low, float center, float high)
            {
                if(value <= center) return 0.5f * smoothstep(low, center, value);
                return 0.5f + 0.5f * smoothstep(center, high, value);
            }

            static float sampleweather(float x, float y)
            {
                if(weatherwarpamplitude > 0.0f) weatherwarp.DomainWarp(x, y);
                return clamp(0.5f + 0.5f * weathernoise.GetNoise(x, y), 0.0f, 1.0f);
            }

            static float coveragefromweather(float value)
            {
                const float clearthreshold = clamp(weatherclearthreshold, 0.0f, 1.0f);
                const float fairthreshold = clamp(weatherfairthreshold, clearthreshold, 1.0f);
                const float overcastthreshold = clamp(weatherovercastthreshold, fairthreshold, 1.0f);
                const float maxhalfwidth = 0.5f * min(fairthreshold - clearthreshold, overcastthreshold - fairthreshold);
                const float halfwidth = min(weathertransitionwidth * 0.5f, max(maxhalfwidth, 0.0f));
                const float fairlow = clearthreshold + halfwidth, fairhigh = overcastthreshold - halfwidth;
                const float faircenter = clamp(fairthreshold, fairlow, fairhigh);

                const float fairminimum = clamp(weatherfairmincoverage, 0.0f, 1.0f);
                const float fairmaximum = clamp(weatherfairmaxcoverage, fairminimum, 1.0f);
                const float overcastcoverage = clamp(weatherovercastcoverage, fairmaximum, 1.0f);
                const float faircoverage = fairminimum + (fairmaximum - fairminimum) * fairprogress(value, fairlow, faircenter, fairhigh);
                const float clearblend = smoothstep(clearthreshold - halfwidth, clearthreshold + halfwidth, value);
                const float overcastblend = smoothstep(overcastthreshold - halfwidth, overcastthreshold + halfwidth, value);
                const float coverage = faircoverage * clearblend;
                return coverage + (overcastcoverage - coverage) * overcastblend;
            }

            static void currentweatherposition(float &x, float &y)
            {
                const int weatherstep = environment::gettimemillis() / max(cloudupdateinterval, 1);
                const float weatherseconds = weatherstep * cloudupdateinterval / 1000.0f;
                const float angle = cloudwindangle * RAD;
                x -= cosf(angle) * weatherwindspeed * weatherseconds;
                y -= sinf(angle) * weatherwindspeed * weatherseconds;
            }
        }

        void update(int seed)
        {
            const uint hash = settingshash(seed);
            if(seed == noiseseed && hash == currentsettingshash) return;

            noiseseed = seed;
            currentsettingshash = hash;
            ++settingsversion;

            weathernoise.SetSeed(int(mixhash(uint(seed) ^ 0xA341316CU)));
            weathernoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
            weathernoise.SetFractalType(FastNoiseLite::FractalType_FBm);
            weathernoise.SetFractalOctaves(2);
            weathernoise.SetFractalLacunarity(2.0f);
            weathernoise.SetFractalGain(0.35f);
            weathernoise.SetFrequency(weatherscale);

            weatherwarp.SetSeed(int(mixhash(uint(seed) ^ 0x9E3779B9U)));
            weatherwarp.SetDomainWarpType(FastNoiseLite::DomainWarpType_OpenSimplex2);
            weatherwarp.SetFractalType(FastNoiseLite::FractalType_None);
            weatherwarp.SetFrequency(weatherwarpscale);
            weatherwarp.SetDomainWarpAmp(weatherwarpamplitude);
        }

        void reset()
        {
            noiseseed = INT_MIN;
            currentsettingshash = 0;
            ++settingsversion;
        }

        int getsettingsversion()
        {
            return settingsversion;
        }

        float samplecoverage(float x, float y)
        {
            return coveragefromweather(sampleweather(x, y));
        }

        ICOMMAND(getdebugweather, "", (),
        {
            if(!player1)
            {
                result("Unavailable");
                return;
            }

            update(getworldseed());
            vec position = player1->o;
            worldpositiontoabsolute(position);
            currentweatherposition(position.x, position.y);

            const float value = sampleweather(position.x, position.y);
            const float clearthreshold = clamp(weatherclearthreshold, 0.0f, 1.0f);
            const float fairthreshold = clamp(weatherfairthreshold, clearthreshold, 1.0f);
            const float overcastthreshold = clamp(weatherovercastthreshold, fairthreshold, 1.0f);
            const char *state = value < clearthreshold ? "Clear" : value < overcastthreshold ? "Fair / Cumulus" : "Overcast";
            defformatstring(formatted, "%s (%.0f%% cloud coverage)", state, coveragefromweather(value) * 100.0f);
            result(formatted);
        });
    }
}
