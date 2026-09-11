#include "game.h"
#ifndef STANDALONE
#include "ambient.h"
#include "weather.h"
#include "world.h"

extern int mainmenu;
extern float cloudwindspeed;
extern bool sampleworldcolumnroof(const ivec &position, int &roof);
extern bool sampleworldsolid(const ivec &position, int &bottom);
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
        FVARP(ambientwindscale, 0.01f, 10, 100);
        FVARP(ambientdepthscale, 1, 64, 256);
        FVARP(ambientaltitudewind, 0, 0.6f, 1);
        FVARP(ambientradius, 640, 1024, 4096);
        VARP(ambientphysical, 0, 1, 1);
        FVARP(ambientphysicalvolume, 0, 0.8f, 1);
        FVARP(ambientphysicalsmallgain, 0, 0.2f, 1);
        FVARP(ambientdropinterval, 0.5f, 4, 120);
        FVARP(ambientrockinterval, 0.5f, 7, 120);
        FVARP(ambientbigrockinterval, 5, 45, 300);

        static float smooth(float value)
        {
            value = clamp(value, 0.0f, 1.0f);
            return value * value * (3 - 2 * value);
        }

        static float blockunits()
        {
            return 1.0f / (worldpositionheight(1) - worldpositionheight(0));
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
            float gain;
            bool selected;
            Voice() : key(0, 0, -1), position(0, 0, 0), handle(0), type(0), nextStart(0), gain(0), selected(false)
            {
            }
        };

        struct PhysicalVoice
        {
            uint handle;
            vec position;
            float gain;
            int kind, variant;
            PhysicalVoice() : handle(0), position(0, 0, 0), gain(0), kind(0), variant(0)
            {
            }
        };

        static int physicaldelay(int kind, bool cave, uint seed)
        {
            float seconds = kind == 0 ? ambientdropinterval : kind == 1 ? ambientrockinterval : ambientbigrockinterval;
            return int(seconds * (0.65f + (mix(seed) & 65535) / 65535.0f * 0.7f) * (cave ? 1000 : 4000) + 0.5f);
        }

        static bool physicalrange(int kind, float distance)
        {
            return distance >= 500 && distance <= 900;
        }

        class AmbientManager
        {
        public:
            vector<Site> sites;
            hashtable<ivec, int> indices;
            Voice voices[96];
            PhysicalVoice physical[4];
            int nextPhysical[3];
            uint physicalSerial[3];
            worldgenerator *terrain;
            int cursor, epoch, nextMix, nextDebug;
            int waiting;

            AmbientManager() : terrain(NULL), cursor(0), epoch(1), nextMix(0), nextDebug(0), waiting(0)
            {
                loopi(3) { nextPhysical[i] = 0; physicalSerial[i] = 0; }
            }

            void reset()
            {
                loopi(4) { stopambientloop(physical[i].handle); physical[i] = PhysicalVoice(); }
                loopi(3) { nextPhysical[i] = 0; physicalSerial[i] = 0; }
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
                    const float temperature = terrain->environmentclimate.gettemperature(placement.position),
                                humidity = terrain->gethumidity(placement.position);
                    site.temperature = clamp((temperature + 20.0f) / 60.0f, 0.0f, 1.0f);
                    site.vegetation = treesuitability(temperature, humidity);
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

            int chooseType(const Site &site, float daylight, float wind, float rain) const
            {
                int chosen = -1;
                float best = 1e16f;
                loop(type, 11)
                {
                    float gain = weight(site, type, daylight, wind, rain);
                    if(gain < 0.001f) continue;
                    // Stable weighted selection gives nearby sources variety without rerolling every update.
                    double random = ((mix(site.seed + type) >> 8) + 1.0) / 16777217.0;
                    float score = float(-log(random) / gain);
                    if(score < best) { best = score; chosen = type; }
                }
                return chosen;
            }

            void select(const vec &listener)
            {
                const int capacity = min(ambientvoices, max(sound::soundchans - 8, 1));
                vector<Voice> desired;
                waiting = 0;
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
                    int type = chooseType(site, daylight, wind, rain);
                    if(type < 0) continue;
                    loopj(8) if(distance < distances[type][j])
                    {
                        for(int k = 7; k > j; --k)
                        {
                            nearest[type][k] = nearest[type][k - 1];
                            distances[type][k] = distances[type][k - 1];
                        }
                        nearest[type][j] = i;
                        distances[type][j] = distance;
                        break;
                    }
                }
                loop(rank, 8) loop(type, 11)
                {
                    if(nearest[type][rank] < 0) continue;
                    Site &site = sites[nearest[type][rank]];
                    float gain = ambientenabled ? ambientvolume * 0.3f : 0;
                    if(gain < 0.001f) continue;
                    if(desired.length() >= capacity) continue;
                    Voice &candidate = desired.add();
                    candidate.key = site.placement.key;
                    candidate.position = site.placement.position;
                    candidate.type = type;
                    candidate.gain = gain;
                }
                // Preserve all retained voices before releasing slots for newly selected sources.
                loopi(96) voices[i].selected = false;
                loopv(desired) loopj(capacity)
                    if(voices[j].gain > 0 && voices[j].key == desired[i].key && voices[j].type == desired[i].type &&
                       voices[j].position.dist(desired[i].position) < 1)
                    {
                        voices[j].selected = desired[i].selected = true;
                        voices[j].gain = desired[i].gain;
                        break;
                    }
                loopi(96) if(!voices[i].selected)
                {
                    stopambientloop(voices[i].handle);
                    voices[i] = Voice();
                }
                loopv(desired) if(!desired[i].selected)
                {
                    bool allocated = false;
                    loopj(capacity) if(!voices[j].selected)
                    {
                        voices[j] = desired[i];
                        voices[j].selected = true;
                        allocated = true;
                        break;
                    }
                    if(!allocated) ++waiting;
                }
            }

            void physicalevents(const vec &listener)
            {
                int roof = -1;
                const bool cave = sampleworldcolumnroof(ivec(camera1->o), roof) && roof >= 0;
                loop(kind, 3)
                {
                    uint seed = placementseed(getworldseed(), ivec(int(floorf(listener.x / 256)),
                                                                  int(floorf(listener.y / 256)), kind));
                    seed = mix(seed ^ physicalSerial[kind]);
                    if(!nextPhysical[kind])
                    {
                        nextPhysical[kind] = totalmillis + physicaldelay(kind, cave, seed);
                        continue;
                    }
                    if(totalmillis < nextPhysical[kind]) continue;
                    ++physicalSerial[kind];
                    nextPhysical[kind] = totalmillis + physicaldelay(kind, cave, seed);
                    if(!ambientenabled || !ambientphysical || (!cave && kind == 0)) continue;
                    int slot = -1;
                    loopi(4) if(!physical[i].handle) { slot = i; break; }
                    if(slot < 0) continue;
                    // Deterministic ranking is independent of streamed site insertion order.
                    int chosen = -1;
                    uint best = ~0U;
                    loopv(sites)
                    {
                        const Site &site = sites[i];
                        if(site.placement.cave != cave || site.water || !physicalrange(kind, listener.dist(site.placement.position))) continue;
                        uint rank = mix(seed ^ site.seed);
                        if(chosen >= 0 && rank >= best) continue;
                        chosen = i;
                        best = rank;
                    }
                    if(chosen < 0) continue;
                    PhysicalVoice &voice = physical[slot];
                    voice.position = sites[chosen].placement.position;
                    voice.kind = kind;
                    voice.variant = 1 + mix(seed + 17) % (kind == 2 ? 2 : 3);
                    voice.gain = ambientvolume * ambientphysicalvolume * (0.75f + (mix(seed + 31) & 65535) / 65535.0f * 0.25f);
                    if(kind != 2) voice.gain *= ambientphysicalsmallgain;
                    vec local(voice.position);
                    worldpositiontolocal(local);
                    int bottom;
                    if(sampleworldsolid(ivec(local), bottom)) continue;
                    defformatstring(name, "%s_%d", kind == 0 ? "water_drop" : kind == 1 ? "rock" : "rock_big", voice.variant);
                    voice.handle = startphysicalsound(name, local, seed, kind == 2 ? 1536 : 1100, voice.gain);
                }
            }

            void update()
            {
                if(mainmenu || !player1 || !camera1) { if(terrain) reset(); return; }
                if(terrain && terrain->seed != getworldseed()) reset();
                if(!terrain) terrain = new worldgenerator(getworldseed());
                scan();
                vec listener(camera1->o);
                worldpositiontoabsolute(listener);
                if(totalmillis >= nextMix)
                {
                    nextMix = totalmillis + 250;
                    weather::update(getworldseed());
                    select(listener);
                    physicalevents(listener);
                }
                loopi(4) if(physical[i].handle)
                {
                    PhysicalVoice &voice = physical[i];
                    vec local(voice.position);
                    worldpositiontolocal(local);
                    if(!ambientenabled || !ambientphysical) { stopambientloop(voice.handle); voice.handle = 0; }
                    else if(!updateambientloop(voice.handle, voice.gain, &local)) voice.handle = 0;
                }
                loopi(96)
                {
                    Voice &voice = voices[i];
                    if(!voice.selected) continue;
                    vec local(voice.position);
                    worldpositiontolocal(local);
                    if(!updateambientloop(voice.handle, voice.gain, &local))
                    {
                        voice.handle = 0;
                        if(totalmillis >= voice.nextStart)
                        {
                            voice.handle = startambientloop(names[voice.type], &local,
                                                            mix(placementseed(getworldseed(), voice.key) + voice.type), int(ambientradius), voice.gain);
                            voice.nextStart = totalmillis + 1000 + i * 13;
                        }
                    }
                }
            }

            void debugparticles(const vec &listener)
            {
                if(debugambient && totalmillis >= nextDebug)
                {
                    loopi(4) if(physical[i].handle)
                    {
                        vec local(physical[i].position);
                        worldpositiontolocal(local);
                        defformatstring(label, "%s_%d (one shot)", physical[i].kind == 0 ? "water_drop" :
                                        physical[i].kind == 1 ? "rock" : "rock_big", physical[i].variant);
                        particle_textcopy(local, label, PART_TEXT, 300, 0x88BBFF, 2);
                        particle_splash(PART_SPARK, 1, 100, local, 0x88BBFF, 2, 1, 0);
                    }
                    nextDebug = totalmillis + 250;
                    bool playing[96];
                    float occlusions[96], gains[96];
                    loopi(96) playing[i] = ambientloopocclusion(voices[i].handle, occlusions[i], gains[i]);
                    loopv(sites)
                    {
                        Site &site = sites[i];
                        if(listener.dist(site.placement.position) > ambientradius * 2) continue;
                        vec local(site.placement.position);
                        worldpositiontolocal(local);
                        string label = "";
                        loop(type, 11)
                        {
                            if(site.placement.cave ? type < 7 : site.water ? type > 3 && type != 6 : type > 5) continue;
                            bool active = false;
                            loopj(96) if(playing[j] && voices[j].type == type && voices[j].key == site.placement.key &&
                                         voices[j].position.dist(site.placement.position) < 1)
                            {
                                active = true;
                                break;
                            }
                            defformatstring(entry, "%s\fc%s%s", label[0] ? "\fc888 / " : "", active ? "8F8" : "888", names[type]);
                            concatstring(label, entry);
                        }
                        particle_splash(PART_SPARK, 1, 100, local, site.placement.cave ? 0xFFAA44 : 0x44FFAA, 2, 1, 0);
                        particle_textcopy(local, label, PART_TEXT, 300, 0x888888, 2);
                    }
                    loopi(96) if(playing[i])
                    {
                        vec local(voices[i].position);
                        worldpositiontolocal(local);
                        local.z += 8 + voices[i].type * 4;
                        defformatstring(label, "%s: blocked %.0f%%, transmitted %.0f%%", names[voices[i].type],
                                        occlusions[i] * 100, gains[i] * 100);
                        particle_textcopy(local, label, PART_TEXT, 300, 0x88FF88, 1);
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
            if(!debugambient || !camera1 || mainmenu) return;
            vec listener(camera1->o);
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
