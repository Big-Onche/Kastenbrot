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
// Let overcast establish before precipitation begins, then fade rain and snow in farther inside the weather system.
FVAR(weatherprecipitationovercastinset, 0.0f, 0.02f, 0.25f);
FVAR(weatherprecipitationtransition, 0.001f, 0.04f, 0.25f);

FVAR(weatherprecipitationwind, 0.0f, 5.0f, 500.0f);
FVAR(weatherprecipitationsnowdrift, 0.0f, 5.0f, 100.0f);
FVAR(weatherprecipitationrainsize, 0.05f, 2.0f, 4.0f);
FVAR(weatherprecipitationsnowsize, 0.05f, 1.25f, 4.0f);
FVAR(weatherprecipitationsnowblendheight, 1.0f, 20.0f, 100.0f);
VAR(weatherprecipitationsnowgroundtime, 0, 1500, 30000);
VAR(weatherprecipitationsnowgroundfade, 100, 4000, 30000);

extern int mainmenu;
extern int worldsnowheight;
extern float cloudwindspeed, cloudwindangle;

namespace game
{
    extern int getworldseed();

    namespace weather
    {
        namespace
        {
            enum
            {
                WEATHER_MAP_VERSION = 1,
                WEATHER_MAP_SIZE = 2048,
                WEATHER_MAP_CELL_SIZE = 512
            };

            static FastNoiseLite weathernoise, weatherwarp;
            static vector<uchar> weathermap;
            static string currentmapfolder = "";
            static int mapseed = INT_MIN, settingsversion = 0, coverageversion = 0;
            static uint currentsettingshash = 0, currentcoveragehash = 0;
            static float rainbudget = 0.0f, snowbudget = 0.0f;
            static bool synchronized = false;
            static int synchronizedseed = 0, synchronizedat = 0;
            static double synchronizedmillis = 0.0;
            static float synchronizedcloudspeed = 0.0f, synchronizedwindangle = 0.0f;

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

            static float samplegeneratedweather(float x, float y)
            {
                if(weatherwarpamplitude > 0.0f) weatherwarp.DomainWarp(x, y);
                return clamp(0.5f + 0.5f * weathernoise.GetNoise(x, y), 0.0f, 1.0f);
            }

            static void setupweathernoise(int seed)
            {
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

            static void generateweathermap(int seed)
            {
                setupweathernoise(seed);
                weathermap.setsize(0);
                const int total = WEATHER_MAP_SIZE * WEATHER_MAP_SIZE;
                weathermap.growbuf(total);
                loopi(total) weathermap.add(0);

                const float halfspan = WEATHER_MAP_SIZE * WEATHER_MAP_CELL_SIZE * 0.5f;
                for(int y = 0; y < WEATHER_MAP_SIZE; ++y)
                {
                    const float worldy = (y + 0.5f) * WEATHER_MAP_CELL_SIZE - halfspan;
                    for(int x = 0; x < WEATHER_MAP_SIZE; ++x)
                    {
                        const float worldx = (x + 0.5f) * WEATHER_MAP_CELL_SIZE - halfspan;
                        const float value = samplegeneratedweather(worldx, worldy);
                        weathermap[y * WEATHER_MAP_SIZE + x] = uchar(clamp(int(value * 255.0f + 0.5f), 0, 255));
                    }
                }
                mapseed = seed;
                currentmapfolder[0] = '\0';
                ++settingsversion;
                ++coverageversion;
            }

            static float sampleweather(float x, float y)
            {
                if(weathermap.length() != WEATHER_MAP_SIZE * WEATHER_MAP_SIZE) return 0.5f;

                const double offset = WEATHER_MAP_SIZE * 0.5 - 0.5;
                const double mapx = double(x) / WEATHER_MAP_CELL_SIZE + offset;
                const double mapy = double(y) / WEATHER_MAP_CELL_SIZE + offset;
                const double wrappedx = mapx - floor(mapx / WEATHER_MAP_SIZE) * WEATHER_MAP_SIZE;
                const double wrappedy = mapy - floor(mapy / WEATHER_MAP_SIZE) * WEATHER_MAP_SIZE;
                const int x0 = int(floor(wrappedx)), y0 = int(floor(wrappedy));
                const int x1 = (x0 + 1) % WEATHER_MAP_SIZE, y1 = (y0 + 1) % WEATHER_MAP_SIZE;
                const float tx = float(wrappedx - x0), ty = float(wrappedy - y0);
                const float v00 = weathermap[y0 * WEATHER_MAP_SIZE + x0] / 255.0f;
                const float v10 = weathermap[y0 * WEATHER_MAP_SIZE + x1] / 255.0f;
                const float v01 = weathermap[y1 * WEATHER_MAP_SIZE + x0] / 255.0f;
                const float v11 = weathermap[y1 * WEATHER_MAP_SIZE + x1] / 255.0f;
                return (v00 + (v10 - v00) * tx) + ((v01 + (v11 - v01) * tx) - (v00 + (v10 - v00) * tx)) * ty;
            }

            static float sampleadvectedweather(float x, float y)
            {
                const float seconds = float(gettimemillis() / 1000.0);
                const float angle = getwindangle(cloudwindangle) * RAD;
                const float distance = getcloudspeed(cloudwindspeed) * seconds;
                return sampleweather(x - cosf(angle) * distance, y - sinf(angle) * distance);
            }

            static bool loadweathermap(const char *folder, int seed)
            {
                if(!folder || !*folder) return false;
                defformatstring(name, "media/map/%s/weather.map", folder);
                stream *file = openfile(path(name), "rb");
                if(!file) return false;

                char magic[4];
                const bool validheader = file->read(magic, sizeof(magic)) == sizeof(magic) && !memcmp(magic, "CCWM", sizeof(magic));
                const uint version = file->getlil<uint>(), storedseed = file->getlil<uint>(), size = file->getlil<uint>(),
                           cellsize = file->getlil<uint>();
                if(!validheader || version != WEATHER_MAP_VERSION || storedseed != uint(seed) || size != WEATHER_MAP_SIZE ||
                   cellsize != WEATHER_MAP_CELL_SIZE)
                {
                    delete file;
                    conoutf(CON_WARN, "weather map %s is incompatible; regenerating it", name);
                    return false;
                }

                const int total = WEATHER_MAP_SIZE * WEATHER_MAP_SIZE;
                weathermap.setsize(0);
                weathermap.growbuf(total);
                loopi(total) weathermap.add(0);
                const bool loaded = file->read(weathermap.getbuf(), total) == size_t(total);
                delete file;
                if(!loaded)
                {
                    weathermap.setsize(0);
                    conoutf(CON_WARN, "weather map %s is truncated; regenerating it", name);
                    return false;
                }
                mapseed = seed;
                copystring(currentmapfolder, folder);
                ++settingsversion;
                ++coverageversion;
                conoutf("loaded fixed weather map %s", name);
                return true;
            }

            static bool saveweathermap(const char *folder)
            {
                if(!folder || !*folder || mapseed == INT_MIN || weathermap.length() != WEATHER_MAP_SIZE * WEATHER_MAP_SIZE) return false;
                defformatstring(name, "media/map/%s/weather.map", folder);
                stream *file = openfile(path(name), "wb");
                if(!file)
                {
                    conoutf(CON_WARN, "could not save fixed weather map to %s", name);
                    return false;
                }
                const bool saved = file->write("CCWM", 4) == 4 && file->putlil<uint>(WEATHER_MAP_VERSION) && file->putlil<uint>(uint(mapseed)) &&
                                   file->putlil<uint>(WEATHER_MAP_SIZE) && file->putlil<uint>(WEATHER_MAP_CELL_SIZE) &&
                                   file->write(weathermap.getbuf(), weathermap.length()) == size_t(weathermap.length()) && file->flush();
                delete file;
                if(!saved) conoutf(CON_WARN, "could not finish saving fixed weather map to %s", name);
                else conoutf("saved fixed weather map %s", name);
                return saved;
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

        }

        void update(int seed)
        {
            seed = getseed(seed);
            if(seed != mapseed || weathermap.length() != WEATHER_MAP_SIZE * WEATHER_MAP_SIZE) generateweathermap(seed);
            const uint hash = settingshash(seed);
            if(hash == currentsettingshash) return;

            currentsettingshash = hash;
            ++settingsversion;
            if(hash != currentcoveragehash)
            {
                currentcoveragehash = hash;
                ++coverageversion;
            }
        }

        bool preparemap(const char *folder, int seed)
        {
            if(folder && !strcmp(currentmapfolder, folder) && mapseed == seed &&
               weathermap.length() == WEATHER_MAP_SIZE * WEATHER_MAP_SIZE) return true;
            if(loadweathermap(folder, seed)) return true;
            generateweathermap(seed);
            if(folder) copystring(currentmapfolder, folder);
            return saveweathermap(folder);
        }

        void reset()
        {
            currentsettingshash = 0;
            rainbudget = snowbudget = 0.0f;
            ++settingsversion;
        }

        void clearsync()
        {
            synchronized = false;
            synchronizedseed = synchronizedat = 0;
            synchronizedmillis = 0.0;
            synchronizedcloudspeed = synchronizedwindangle = 0.0f;
            reset();
        }

        void synctime(int seed, uint millis, float cloudspeed, float windangle)
        {
            const bool settingschanged = !synchronized || synchronizedseed != seed || synchronizedcloudspeed != cloudspeed ||
                                         synchronizedwindangle != windangle;
            synchronized = true;
            synchronizedseed = seed;
            synchronizedat = totalmillis;
            synchronizedmillis = double(millis) + getserverrtt() * 0.5;
            synchronizedcloudspeed = max(cloudspeed, 0.0f);
            synchronizedwindangle = windangle;
            if(settingschanged) reset();
        }

        int getseed(int fallback)
        {
            return synchronized ? synchronizedseed : fallback;
        }

        double gettimemillis()
        {
            if(!synchronized) return environment::gettimemillis();
            return synchronizedmillis + max(totalmillis - synchronizedat, 0);
        }

        float getcloudspeed(float fallback)
        {
            return synchronized ? synchronizedcloudspeed : fallback;
        }

        float getwindangle(float fallback)
        {
            return synchronized ? synchronizedwindangle : fallback;
        }

        int getsettingsversion()
        {
            return settingsversion;
        }

        int getcoverageversion()
        {
            return coverageversion;
        }

        float samplecoverage(float x, float y)
        {
            return coveragefromweather(sampleweather(x, y));
        }

        float samplecurrentovercast(float x, float y)
        {
            const float value = sampleadvectedweather(x, y);
            const float fairthreshold = clamp(weatherfairthreshold, weatherclearthreshold, 1.0f);
            const float overcastthreshold = clamp(weatherovercastthreshold, fairthreshold, 1.0f);
            const float halfwidth = weatherovercastlighttransition * 0.5f;
            return smoothstep(overcastthreshold - halfwidth, overcastthreshold + halfwidth, value);
        }

        static float samplecurrentprecipitation(float x, float y)
        {
            const float value = sampleadvectedweather(x, y);
            const float fairthreshold = clamp(weatherfairthreshold, weatherclearthreshold, 1.0f);
            const float overcastthreshold = clamp(weatherovercastthreshold, fairthreshold, 1.0f);
            const float start = clamp(overcastthreshold + weatherprecipitationovercastinset, overcastthreshold, 1.0f);
            const float end = clamp(start + weatherprecipitationtransition, start, 1.0f);
            return smoothstep(start, end, value);
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
            const float intensity = samplecurrentprecipitation(absolute.x, absolute.y);
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

            const float windangle = getwindangle(cloudwindangle) * RAD;
            const vec wind(cosf(windangle) * weatherprecipitationwind, sinf(windangle) * weatherprecipitationwind, 0.0f);
            loopi(count)
            {
                const float distance = sqrtf(rndscale(1.0f)) * weatherprecipitationradius;
                const float angle = rndscale(2.0f * M_PI);
                vec origin(camera1->o.x + cosf(angle) * distance, camera1->o.y + sinf(angle) * distance,
                           camera1->o.z + weatherprecipitationspawnheight * (1.0f + 0.25f * rndscale(1.0f)));
                vec weatherorigin(origin);
                worldpositiontoabsolute(weatherorigin);
                if(rndscale(1.0f) >= samplecurrentprecipitation(weatherorigin.x, weatherorigin.y)) continue;

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

            const float value = sampleadvectedweather(position.x, position.y);
            const float clearthreshold = clamp(weatherclearthreshold, 0.0f, 1.0f);
            const float fairthreshold = clamp(weatherfairthreshold, clearthreshold, 1.0f);
            const float overcastthreshold = clamp(weatherovercastthreshold, fairthreshold, 1.0f);
            const char *state = value < clearthreshold ? "Clear" : value < overcastthreshold ? "Fair / Cumulus" : "Overcast";
            defformatstring(formatted, "%s (%.0f%% cloud coverage)", state, coveragefromweather(value) * 100.0f);
            result(formatted);
        });
    }
}
