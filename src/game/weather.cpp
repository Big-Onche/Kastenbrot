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

// spawn rates of particles
const float MAIN_RATE = 512.0f,
            RAIN_RATE_MULT = 4.0f,
            SNOW_RATE_MULT = 0.2f;

const int PARTICLE_LIFE = 4000, // doubled for snow
          PARTICLE_SPEED = 2000; // 1/3 for snow

VARP(weatherprecipitation, 0, 1, 1);

// With the default cloud shape scale, weather systems are roughly thirty times wider than a typical cloud feature.
FVAR(weatherscale, 0.000001f, 0.00003f, 0.001f);
FVAR(weatherwarpscale, 0.000001f, 0.000015f, 0.001f);
FVAR(weatherwarpamplitude, 0.0f, 6000.0f, 50000.0f);

// Normalized noise boundaries give about 20% clear, 55% fair, and 25% overcast weather regions, favoring fair cumulus conditions.
FVAR(weatherclearthreshold, 0.0f, 0.35f, 1.0f);
FVAR(weatherfairthreshold, 0.0f, 0.50f, 1.0f);
FVAR(weatherovercastthreshold, 0.0f, 0.625f, 1.0f);
FVAR(weathertransitionwidth, 0.001f, 0.06f, 0.5f);

// Coverage is the desired fraction of the local cloud-shape mask, not another density signal.
FVAR(weatherfairmincoverage, 0.0f, 0.22f, 1.0f);
FVAR(weatherfairmaxcoverage, 0.0f, 0.55f, 1.0f);
FVAR(weatherovercastcoverage, 0.0f, 0.95f, 1.0f);

// Overcast lighting is applied by game/environment.cpp after time-of-day lighting.
FVAR(weatherovercastlightreduction, 0.0f, 0.5f, 1.0f);
FVAR(weatherovercastsunlightreduction, 0.0f, 0.90f, 1.0f);
FVAR(weatherovercastambientgray, 0.0f, 0.85f, 1.0f);
FVAR(weatherovercastsunwhite, 0.0f, 0.90f, 1.0f);
FVAR(weatherovercastlighttransition, 0.001f, 0.10f, 0.5f);

FVAR(weatherprecipitationradius, 16.0f, 512.0f, 1024.0f);
FVAR(weatherprecipitationspawnheight, 16.0f, 256.0f, 512.0f);
FVARP(weatherprecipitationcollisionradius, 0.0f, 256.0f, 512.0f);
VARP(weatherprecipitationmaxspawn, 1, 64, 512);

FVAR(weatherprecipitationwind, 0.0f, 5.0f, 500.0f);
FVAR(weatherprecipitationsnowdrift, 0.0f, 5.0f, 100.0f);
FVAR(weatherprecipitationrainsize, 0.05f, 2.0f, 4.0f);
FVAR(weatherprecipitationsnowsize, 0.05f, 1.25f, 4.0f);
FVAR(weatherprecipitationsnowblendheight, 1.0f, 20.0f, 100.0f);
VAR(weatherprecipitationsnowgroundtime, 0, 1500, 30000);
VAR(weatherprecipitationsnowgroundfade, 100, 4000, 30000);

extern int cloudupdateinterval;
extern int mainmenu;
extern int worldsnowheight;
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
            static float rainbudget = 0.0f, snowbudget = 0.0f;

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
            rainbudget = snowbudget = 0.0f;
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

        float samplecurrentovercast(float x, float y)
        {
            currentweatherposition(x, y);
            const float value = sampleweather(x, y);
            const float fairthreshold = clamp(weatherfairthreshold, weatherclearthreshold, 1.0f);
            const float overcastthreshold = clamp(weatherovercastthreshold, fairthreshold, 1.0f);
            const float halfwidth = weatherovercastlighttransition * 0.5f;
            return smoothstep(overcastthreshold - halfwidth, overcastthreshold + halfwidth, value);
        }

        void addparticles()
        {
            if(!weatherprecipitation || mainmenu || !camera1 || !player1 || curtime <= 0)
            {
                rainbudget = snowbudget = 0.0f;
                return;
            }

            update(getworldseed());
            vec absolute = player1->o;
            absolute.z -= player1->eyeheight;
            const float playerheight = worldpositionheight(absolute.z);
            worldpositiontoabsolute(absolute);
            const float intensity = samplecurrentovercast(absolute.x, absolute.y) * clamp(getworldskyexposure(camera1->o), 0.0f, 1.0f);
            if(intensity <= 1e-3f)
            {
                rainbudget = snowbudget = 0.0f;
                return;
            }

            const float snowrange = max(weatherprecipitationsnowblendheight, 1.0f);
            const float snowblend = smoothstep(worldsnowheight - snowrange, worldsnowheight + snowrange, playerheight);
            const float frameamount = MAIN_RATE * intensity * min(curtime, 100) / 1000.0f;
            rainbudget += frameamount * RAIN_RATE_MULT * (1.0f - snowblend);
            snowbudget += frameamount * SNOW_RATE_MULT * snowblend;
            const int raincount = min(int(rainbudget), weatherprecipitationmaxspawn);
            rainbudget = min(rainbudget - raincount, 1.0f);
            const int snowcount = min(int(snowbudget), weatherprecipitationmaxspawn - raincount);
            snowbudget = min(snowbudget - snowcount, 1.0f);
            const int count = raincount + snowcount;
            if(count <= 0) return;

            const float windangle = cloudwindangle * RAD;
            const vec wind(cosf(windangle) * weatherprecipitationwind, sinf(windangle) * weatherprecipitationwind, 0.0f);
            loopi(count)
            {
                const float distance = sqrtf(rndscale(1.0f)) * weatherprecipitationradius;
                const float angle = rndscale(2.0f * M_PI);
                vec origin(camera1->o.x + cosf(angle) * distance, camera1->o.y + sinf(angle) * distance,
                           camera1->o.z + weatherprecipitationspawnheight * (1.0f + 0.25f * rndscale(1.0f)));
                const bool snow = i >= raincount;
                vec velocity(wind);

                int zvel = snow ? PARTICLE_SPEED * 0.33f : PARTICLE_SPEED;

                if(snow)
                {
                    velocity.x += rndscale(2.0f * weatherprecipitationsnowdrift) - weatherprecipitationsnowdrift;
                    velocity.y += rndscale(2.0f * weatherprecipitationsnowdrift) - weatherprecipitationsnowdrift;
                    velocity.z = -zvel * (0.8f + 0.4f * rndscale(1.0f));
                }
                else velocity.z = -zvel * (0.85f + 0.3f * rndscale(1.0f));

                const bool collide = distance <= weatherprecipitationcollisionradius;
                if(snow) particle_precipitation(PART_SNOW, origin, velocity, PARTICLE_LIFE * 2, 0xFFFFFF, weatherprecipitationsnowsize, 50, collide);
                else particle_precipitation(PART_RAIN, origin, velocity, PARTICLE_LIFE, 0x4062B6, weatherprecipitationrainsize, 0, collide);

            }
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
