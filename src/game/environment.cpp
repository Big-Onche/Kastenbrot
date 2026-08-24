// environment.cpp: Kastenbrot day and night cycle
// reviewed

#include "game.h"
#include "weather.h"

#ifndef STANDALONE

extern bvec ambient, fogcolour, sunlight;
extern float ambientscale, sunlightscale;
extern float sunlightyaw, sunlightpitch;
extern void setsunlightdir();

extern float weatherovercastlightreduction;
extern float weatherovercastsunlightreduction;
extern float weatherovercastambientgray;
extern float weatherovercastsunwhite;

namespace game
{
    extern int getworldseed();

    namespace environment
    {
        static const int CYCLE_MILLIS = 20 * 60 * 1000;

        static const float HOURS_PER_DAY = 24.0f;
        static const float START_HOUR = 8.0f;
        static const float SUNRISE_HOUR = 6.0f;
        static const float DEGREES_PER_HOUR = 360.0f / HOURS_PER_DAY;
        static const float MAX_SUN_PITCH = 70.0f;
        static const float AMBIENT_LIGHT_LEVELS = 16.0f;

        static const int DAY_FOG_COLOR = 0xC0E0F5;
        static const int DAY_AMBIENT_COLOR = 0x5A5A6E;
        static const int NIGHT_FOG_COLOR = 0x0A1026;
        static const int NIGHT_AMBIENT_COLOR = 0x080C20;

        struct lightingkey
        {
            float hour;
            int sunlightcolor, fogcolor, ambientcolor;
            float sunlightintensity;
        };

        // hours must remain strictly increasing
        static const lightingkey lightingkeys[] =
        {
            {  0.0f, 0x8090C0, NIGHT_FOG_COLOR, NIGHT_AMBIENT_COLOR, 0.06f },
            {  5.0f, 0x7180B0, 0x151E38, 0x10172D, 0.05f },
            {  6.0f, 0xFF6A3D, 0x6A5062, 0x302A40, 0.30f },
            {  7.0f, 0xFFC080, 0x887A82, 0x4B4658, 0.75f },
            {  8.0f, 0xFFF8E0, DAY_FOG_COLOR, DAY_AMBIENT_COLOR, 1.00f },
            { 16.0f, 0xFFF8E0, DAY_FOG_COLOR, DAY_AMBIENT_COLOR, 1.00f },
            { 17.0f, 0xFFC080, 0x887A82, 0x4B4658, 0.75f },
            { 18.0f, 0xFF6238, 0x704858, 0x30243A, 0.28f },
            { 19.0f, 0x7180B0, 0x151E38, 0x10172D, 0.05f },
            { 24.0f, 0x8090C0, NIGHT_FOG_COLOR, NIGHT_AMBIENT_COLOR, 0.06f }
        };

        static const int NUM_LIGHTING_KEYS = sizeof(lightingkeys) / sizeof(lightingkeys[0]);

        enum
        {
            LIGHTING_DIRECT = 0,
            LIGHTING_SETVARS
        };

        static double cyclemillis = START_HOUR * CYCLE_MILLIS / HOURS_PER_DAY;
        static bool initialized = false, timefrozen = false;
        static float worldambientlightlevel = 0.0f;

        static float smoothstep(float value)
        {
            value = clamp(value, 0.0f, 1.0f);
            return value * value * (3.0f - 2.0f * value);
        }

        static float interpolate(float from, float to, float amount)
        {
            return from + (to - from) * amount;
        }

        static float luminance(const bvec &color)
        {
            return color.r * 0.2126f + color.g * 0.7152f + color.b * 0.0722f;
        }

        static bvec interpolatecolor(int from, int to, float amount)
        {
            bvec result;
            result.lerp(bvec::hexcolor(from), bvec::hexcolor(to), amount);
            return result;
        }

        static float sampleplayerovercast()
        {
            vec position = player1->o;
            worldpositiontoabsolute(position);
            return weather::samplecurrentovercast(position.x, position.y);
        }

        static bvec neutralizeambient(const bvec &color, float amount)
        {
            const int gray = int(luminance(color) + 0.5f);

            bvec result;
            result.lerp(color, bvec(uchar(gray), uchar(gray), uchar(gray)), amount);
            return result;
        }

        static bvec neutralizesunlight(const bvec &color, float amount)
        {
            bvec result;
            result.lerp(color, bvec(255, 255, 255), amount);
            return result;
        }

        static void applyovercastambient(bvec &color, float overcast)
        {
            if(overcast <= 0.0f) return;

            const float overcastfalloff = 1.0f - clamp(weatherovercastlightreduction, 0.0f, 1.0f) * overcast;
            color = neutralizeambient(color, clamp(weatherovercastambientgray, 0.0f, 1.0f) * overcast);
            loopk(3) color[k] = uchar(clamp(color[k] * overcastfalloff + 0.5f, 0.0f, 255.0f));
        }

        static void applyovercastsunlight(bvec &color, float &scale, float overcast)
        {
            if(overcast <= 0.0f) return;

            const float overcastfalloff = 1.0f - clamp(weatherovercastsunlightreduction, 0.0f, 1.0f) * overcast;
            color = neutralizesunlight(color, clamp(weatherovercastsunwhite, 0.0f, 1.0f) * overcast);
            scale *= overcastfalloff;
        }

        static void applylighting(int mode)
        {
            const float hour = float(cyclemillis * HOURS_PER_DAY / CYCLE_MILLIS);

            const lightingkey *from = &lightingkeys[0];
            const lightingkey *to = &lightingkeys[1];

            loopi(NUM_LIGHTING_KEYS - 1)
            {
                if(hour <= lightingkeys[i + 1].hour)
                {
                    from = &lightingkeys[i];
                    to = &lightingkeys[i + 1];
                    break;
                }
            }

            const float blend = smoothstep((hour - from->hour) / (to->hour - from->hour));

            bvec newsunlight = interpolatecolor(from->sunlightcolor, to->sunlightcolor, blend);
            bvec newfog = interpolatecolor(from->fogcolor, to->fogcolor, blend);
            bvec newambient = interpolatecolor(from->ambientcolor, to->ambientcolor, blend);

            float newsunlightscale = interpolate(from->sunlightintensity, to->sunlightintensity, blend);
            float overcast = 0.0f;

            // weather is refreshed here only when a local player exists, including reset/sync lighting updates
            if(player1)
            {
                weather::update(getworldseed());
                overcast = sampleplayerovercast();
            }

            applyovercastambient(newambient, overcast);
            applyovercastsunlight(newsunlight, newsunlightscale, overcast);

            worldambientlightlevel = luminance(newambient) * ambientscale * (AMBIENT_LIGHT_LEVELS / 255.0f);

            const float orbit = (hour - SUNRISE_HOUR) * DEGREES_PER_HOUR * RAD;

            float newsunlightyaw = hour * DEGREES_PER_HOUR;
            if(newsunlightyaw >= 360.0f) newsunlightyaw -= 360.0f;

            const float newsunlightpitch = sinf(orbit) * MAX_SUN_PITCH;

            if(mode == LIGHTING_SETVARS)
            {
                setvar("sunlight", newsunlight.tohexcolor());
                setvar("fogcolour", newfog.tohexcolor());
                setvar("ambient", newambient.tohexcolor());
                setfvar("sunlightscale", newsunlightscale);
                setfvar("sunlightyaw", newsunlightyaw);
                setfvar("sunlightpitch", newsunlightpitch);
                return;
            }

            sunlight = newsunlight;
            fogcolour = newfog;
            ambient = newambient;

            sunlightscale = newsunlightscale;
            sunlightyaw = newsunlightyaw;
            sunlightpitch = newsunlightpitch;

            setsunlightdir();
        }

        void reset()
        {
            cyclemillis = START_HOUR * CYCLE_MILLIS / HOURS_PER_DAY;
            initialized = true;
            timefrozen = false;

            applylighting(LIGHTING_SETVARS);
        }

        void update()
        {
            if(!initialized) reset();
            if(!timefrozen && curtime > 0)
            {
                cyclemillis += curtime;
                while(cyclemillis >= CYCLE_MILLIS) cyclemillis -= CYCLE_MILLIS;
            }

            applylighting(LIGHTING_DIRECT);
        }

        void synctime(int millis, bool frozen)
        {
            cyclemillis = clamp(millis, 0, CYCLE_MILLIS - 1);
            initialized = true;
            timefrozen = frozen;

            applylighting(LIGHTING_SETVARS);
        }

        // getters
        int gettimemillis() { return int(cyclemillis); }
        float getdayprogress() { return float(cyclemillis / CYCLE_MILLIS); }
        float gethourafter(int millis)
        {
            const double futuremillis = timefrozen ? cyclemillis : fmod(cyclemillis + max(millis, 0), double(CYCLE_MILLIS));
            return float(futuremillis * HOURS_PER_DAY / CYCLE_MILLIS);
        }
        bool istimefrozen() { return timefrozen; }
        float getambientlightlevel() { return worldambientlightlevel; }

        // game commands:
        // /time <hour(0-24)>
        // /time freeze
        ICOMMAND(time, "sN", (char *value, int *numargs),
        {
            if(game::waitforserveredit())
            {
                defformatstring(command, "time %s", *numargs == 1 ? value : "");
                game::requestworldcommand(command);
                return;
            }

            if(*numargs == 1 && cubecaseequal(value, "freeze"))
            {
                const double hour = cyclemillis * HOURS_PER_DAY / CYCLE_MILLIS;
                const int minutes = int(hour * 60.0 + 0.5) % (24 * 60);

                timefrozen = true;
                conoutf(CON_INFO, "time frozen at %02d:%02d", minutes / 60, minutes % 60);

                return;
            }

            char *end = NULL;
            const double hour = *numargs == 1 ? strtod(value, &end) : -1;

            if(*numargs != 1 || end == value || *end || !(hour >= 0 && hour <= 24))
            {
                conoutf(CON_ERROR, "usage: /time <hour 0-24|freeze>");
                return;
            }

            const double normalizedhour = hour == 24 ? 0 : hour;

            cyclemillis = normalizedhour * CYCLE_MILLIS / HOURS_PER_DAY;
            initialized = true;
            timefrozen = false;

            applylighting(LIGHTING_SETVARS);

            const int minutes = int(normalizedhour * 60.0 + 0.5) % (24 * 60);

            conoutf(CON_INFO, "time set to %02d:%02d", minutes / 60, minutes % 60);
        });
    }
}

#endif
