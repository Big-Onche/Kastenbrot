#include "game.h"
#ifndef STANDALONE
#include "ambient.h"
#include "weather.h"
#include "world.h"

extern int mainmenu;
extern float cloudwindspeed;
namespace sound { extern int soundchans; }

namespace game
{
    namespace ambience
    {
        VARP(ambientenabled, 0, 1, 1);
        VAR(debugambient, 0, 0, 1);
        VARP(ambientscanbudget, 16, 256, 2048);
        VARP(ambientvoices, 8, 24, 96);
        FVARP(ambientvolume, 0, 0.65f, 1);
        FVARP(ambientweathertransition, 0.1f, 5, 60);
        FVARP(ambientbiometransition, 0.1f, 10, 60);
        FVARP(ambientcavetransition, 0.1f, 3, 60);
        FVARP(ambientdepthtransition, 0.1f, 8, 60);
        FVARP(ambientwindscale, 0.01f, 10, 100);
        FVARP(ambientdepthscale, 1, 64, 256);
        FVARP(ambientaltitudewind, 0, 0.6f, 1);
        FVARP(ambientradius, 640, 1024, 4096);

        static float smooth(float value)
        {
            value = clamp(value, 0.0f, 1.0f);
            return value * value * (3 - 2 * value);
        }

        static float blockunits()
        {
            return 1.0f / (worldpositionheight(1) - worldpositionheight(0));
        }

        static void approach(float &current, float target, float dt, float seconds)
        {
            current += (target - current) * (1 - expf(-dt / seconds));
        }

        static uint mix(uint value)
        {
            value ^= value >> 16;
            value *= 0x7feb352dU;
            value ^= value >> 15;
            value *= 0x846ca68bU;
            return value ^ (value >> 16);
        }

        static uint placementseed(int seed, const ivec &key)
        {
            return mix(uint(seed) ^ mix(uint(key.x)) ^ mix(uint(key.y) + 37) ^ mix(uint(key.z) + 71));
        }

        static const char *const names[] =
        {
            "calm", "light_wind", "cold_wind", "rain", "birds", "crickets", "waves",
            "cave_1", "cave_2", "deep_cave_1", "deep_cave"
        };

        struct Site
        {
            ambientplacement placement;
            vec sampledPosition;
            uint seed;
            int seen;
            float temperature, vegetation, altitude, depth;
            bool water;
        };

        struct Voice
        {
            ivec key;
            vec position;
            uint handle;
            int type, nextStart;
            float current, target;
            bool selected;
            Voice() : key(0, 0, -1), position(0, 0, 0), handle(0), type(0), nextStart(0), current(0), target(0), selected(false)
            {
            }
        };

        class AmbientManager
        {
        public:
            vector<Site> sites;
            hashtable<ivec, int> indices;
            Voice voices[96];
            worldgenerator *terrain;
            int cursor, epoch, nextMix, nextDebug;
            int waiting;

            AmbientManager() : terrain(NULL), cursor(0), epoch(1), nextMix(0), nextDebug(0), waiting(0)
            {
            }

            void reset()
            {
                loopi(96)
                {
                    stopambientloop(voices[i].handle);
                    voices[i] = Voice();
                }
                sites.setsize(0);
                indices.clear();
                delete terrain;
                terrain = NULL;
                cursor = nextMix = nextDebug = 0;
                epoch = 1;
                waiting = 0;
            }

            void scan()
            {
                const uint started = SDL_GetTicks();
                loopi(ambientscanbudget)
                {
                    if(i && SDL_GetTicks() - started >= 2) break;
                    ambientplacement placement;
                    cursor = scanambientsection(cursor, placement);
                    if(!cursor)
                    {
                        for(int j = sites.length() - 1; j >= 0; --j) if(sites[j].seen != epoch)
                        {
                            indices.remove(sites[j].placement.key);
                            sites.removeunordered(j);
                            if(j < sites.length()) indices[sites[j].placement.key] = j;
                        }
                        ++epoch;
                        break;
                    }
                    if(placement.key.z < 0) continue;
                    int *found = indices.access(placement.key);
                    if(found)
                    {
                        Site &site = sites[*found];
                        site.seen = epoch;
                        if(site.sampledPosition.dist(placement.position) < 1 && site.placement.cave == placement.cave) continue;
                    }
                    const int index = found ? *found : sites.length();
                    if(!found) { sites.add(); indices[placement.key] = index; }
                    Site &site = sites[index];
                    site.placement = placement;
                    site.sampledPosition = placement.position;
                    site.seen = epoch;
                    site.seed = placementseed(getworldseed(), placement.key);
                    const int x = int(floorf(placement.position.x / blockunits()));
                    const int y = int(floorf(placement.position.y / blockunits()));
                    float moisture;
                    terrain->climate(x, y, site.temperature, moisture);
                    site.temperature = clamp(0.5f + 0.5f * site.temperature, 0.0f, 1.0f);
                    site.vegetation = smooth(0.5f + 0.5f * moisture);
                    const float height = worldpositionheight(placement.position.z) - (placement.cave ? 0 : 5);
                    const float low = min(terrain->settings.stonelow, terrain->settings.stonehigh);
                    const float high = max(terrain->settings.stonelow, terrain->settings.stonehigh);
                    site.altitude = smooth((height - low) / max(high - low, 1.0f));
                    const int surface = terrain->height(x, y);
                    site.water = surface < terrain->settings.sealevel;
                    if(site.water && !placement.cave)
                        site.placement.position.z = max(site.placement.position.z,
                                                        (terrain->settings.sealevel - worldpositionheight(0) + 5) * blockunits());
                    site.depth = smooth(max(surface - height, 0.0f) / ambientdepthscale);
                }
            }

            float weight(const Site &site, int type, float daylight, float wind, float rain) const
            {
                if(site.placement.cave)
                {
                    float deep = site.depth;
                    switch(type)
                    {
                        case 7: return (1 - deep) * (1 - 0.5f * deep);
                        case 8: return (1 - deep) * deep;
                        case 9: return deep * (1 - 0.5f * deep);
                        case 10: return deep * deep * 0.5f;
                        default: return 0;
                    }
                }
                switch(type)
                {
                    case 0: return 0.3f * (1 - rain);
                    case 1: return wind * (1 - 0.5f * site.altitude);
                    case 2: return max(wind * (1 - site.temperature), site.altitude * ambientaltitudewind);
                    case 3: return rain;
                    case 4: return site.water ? 0 : daylight * site.vegetation * (1 - site.altitude) * (1 - rain);
                    case 5: return site.water ? 0 : (1 - daylight) * site.temperature * (1 - site.altitude) * (1 - rain);
                    case 6: return site.water ? 1 : 0;
                    default: return 0;
                }
            }

            void select(const vec &listener)
            {
                const int capacity = min(ambientvoices, max(sound::soundchans - 8, 1));
                const int desiredlimit = max(capacity - max(capacity / 4, 1), 1);
                int desired = 0;
                waiting = 0;
                loopi(96)
                {
                    voices[i].selected = false;
                    // These sources are already inaudible: release their physical channels immediately.
                    if(listener.dist(voices[i].position) >= ambientradius)
                    {
                        stopambientloop(voices[i].handle);
                        voices[i] = Voice();
                    }
                }
                float daylight = smooth(0.5f + 2 * sinf((environment::getdayprogress() - 0.25f) * 2 * M_PI));
                float wind = clamp(fabsf(weather::getcloudspeed(cloudwindspeed)) / ambientwindscale, 0.0f, 1.0f);
                // Round-robin families reserve coverage for caves and waves even near a busy surface.
                int nearest[11][8];
                float distances[11][8];
                loop(t, 11) loopi(8) { nearest[t][i] = -1; distances[t][i] = ambientradius; }
                loopv(sites)
                {
                    Site &site = sites[i];
                    float distance = listener.dist(site.placement.position);
                    if(distance >= ambientradius) continue;
                    float rain = weather::samplecurrentrain(site.placement.position.x, site.placement.position.y,
                                                           worldpositionheight(site.placement.position.z));
                    loop(t, 11)
                    {
                        if(weight(site, t, daylight, wind, rain) < 0.001f) continue;
                        loopj(8) if(distance < distances[t][j])
                        {
                            for(int k = 7; k > j; --k) { nearest[t][k] = nearest[t][k - 1]; distances[t][k] = distances[t][k - 1]; }
                            nearest[t][j] = i;
                            distances[t][j] = distance;
                            break;
                        }
                    }
                }
                loop(rank, 8) loop(type, 11)
                {
                    if(nearest[type][rank] < 0) continue;
                    Site &site = sites[nearest[type][rank]];
                    float rain = weather::samplecurrentrain(site.placement.position.x, site.placement.position.y,
                                                           worldpositionheight(site.placement.position.z));
                    float gain = ambientenabled ? weight(site, type, daylight, wind, rain) * ambientvolume * 0.3f : 0;
                    if(gain < 0.001f) continue;
                    // Count desired sites even when allocation fails. Otherwise old, lower-ranked
                    // voices reselect themselves and permanently starve newly streamed sites.
                    if(desired++ >= desiredlimit) continue;
                    Voice *voice = NULL;
                    loopj(capacity) if(voices[j].key == site.placement.key && voices[j].type == type &&
                                           voices[j].position.dist(site.placement.position) < 1) { voice = &voices[j]; break; }
                    if(!voice) loopj(capacity) if(!voices[j].selected && voices[j].current < 0.001f)
                    {
                        voice = &voices[j];
                        stopambientloop(voice->handle);
                        *voice = Voice();
                        voice->key = site.placement.key;
                        voice->type = type;
                        voice->position = site.placement.position;
                        voice->nextStart = totalmillis + int(mix(site.seed + type) % 750);
                        break;
                    }
                    if(!voice) { ++waiting; continue; }
                    voice->selected = true;
                    voice->target = gain;
                }
                loopi(96) if(!voices[i].selected) voices[i].target = 0;
            }

            void update()
            {
                if(mainmenu || !player1) { if(terrain) reset(); return; }
                if(terrain && terrain->seed != getworldseed()) reset();
                if(!terrain) terrain = new worldgenerator(getworldseed());
                scan();
                vec listener(player1->o);
                worldpositiontoabsolute(listener);
                if(totalmillis >= nextMix)
                {
                    nextMix = totalmillis + 250;
                    weather::update(getworldseed());
                    select(listener);
                }
                loopi(96)
                {
                    Voice &voice = voices[i];
                    float transition = voice.type >= 9 ? ambientdepthtransition : voice.type >= 7 ? ambientcavetransition :
                                       voice.type >= 4 ? ambientbiometransition : ambientweathertransition;
                    if(!voice.selected) transition = 0.35f;
                    approach(voice.current, voice.target, max(curtime, 0) / 1000.0f, transition);
                    vec local(voice.position);
                    worldpositiontolocal(local);
                    if(!updateambientloop(voice.handle, voice.current, &local))
                    {
                        voice.handle = 0;
                        if(voice.target > 0.001f && totalmillis >= voice.nextStart)
                        {
                            voice.handle = startambientloop(names[voice.type], &local,
                                                            mix(placementseed(getworldseed(), voice.key) + voice.type), int(ambientradius));
                            voice.nextStart = totalmillis + 1000 + i * 13;
                        }
                    }
                    if(!voice.selected && voice.current < 0.001f)
                    {
                        stopambientloop(voice.handle);
                        voice = Voice();
                    }
                }
            }

            void debugparticles(const vec &listener)
            {
                if(debugambient && totalmillis >= nextDebug)
                {
                    nextDebug = totalmillis + 250;
                    loopv(sites)
                    {
                        Site &site = sites[i];
                        if(listener.dist(site.placement.position) > ambientradius * 2) continue;
                        vec local(site.placement.position);
                        worldpositiontolocal(local);
                        const char *label = site.placement.cave ? "cave_1 / cave_2 / deep_cave_1 / deep_cave_2" :
                                            site.water ? "waves / calm / light_wind / cold_wind / rain" :
                                            "calm / light_wind / cold_wind / rain / birds / crickets";
                        particle_splash(PART_SPARK, 1, 100, local, site.placement.cave ? 0xFFAA44 : 0x44FFAA, 2, 1, 0);
                        particle_textcopy(local, label, PART_TEXT, 300, 0xFFFFFF, 2);
                    }
                    loopi(96) if(voices[i].handle)
                    {
                        float occlusion, gain;
                        if(!ambientloopocclusion(voices[i].handle, occlusion, gain)) continue;
                        vec local(voices[i].position);
                        worldpositiontolocal(local);
                        local.z += 8 + voices[i].type * 4;
                        defformatstring(label, "%s: blocked %.0f%%, transmitted %.0f%%", names[voices[i].type],
                                        occlusion * 100, gain * 100);
                        particle_textcopy(local, label, PART_TEXT, 300, occlusion > 0 ? 0xFF8866 : 0x88FF88, 1);
                    }
                }
            }
        };

        static AmbientManager manager;
        void update()
        {
            manager.update();
        }
        void reset()
        {
            manager.reset();
        }
        void addparticles()
        {
            if(!debugambient || !player1 || mainmenu) return;
            vec listener(player1->o);
            worldpositiontoabsolute(listener);
            manager.debugparticles(listener);
        }
        void drawhud()
        {
            if(!debugambient) return;
            int active = 0, pending = manager.waiting;
            loopi(96)
            {
                if(manager.voices[i].handle) ++active;
                else if(manager.voices[i].selected) ++pending;
            }
            defformatstring(line, "Ambient: %d sites, %d playing, %d pending, radius %.0f cubes, scan %d", manager.sites.length(), active,
                            pending, ambientradius / blockunits(), manager.cursor);
            draw_text(line, 20, 100);
        }
    }
}
#endif
