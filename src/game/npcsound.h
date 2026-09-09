#ifndef NPCSOUND_H
#define NPCSOUND_H

namespace npcvoice
{
    inline unsigned int mix(unsigned int value)
    {
        value ^= value >> 16;
        value *= 0x7FEB352DU;
        value ^= value >> 15;
        value *= 0x846CA68BU;
        return value ^ (value >> 16);
    }

    // A separate counter-based stream: AI, rendering, and other mobs never
    // consume this voice's random numbers. Deadlines advance from deadlines,
    // not frames, so frame rate does not change the sequence or cadence.
    struct schedule
    {
        unsigned long long next;
        unsigned int serial, previous;
        int variant;
        bool initialized;

        schedule() : next(0), serial(0), previous(0), variant(0), initialized(false) {}

        template<class Sounds> void advance(unsigned int seed, const Sounds &sounds)
        {
            const unsigned int event = mix(seed ^ mix(serial++));
            variant = mix(event ^ 0x68E31DA4U) % sounds.length();
            const int minimum = sounds[variant].minmillis, maximum = sounds[variant].maxmillis;
            next += minimum + mix(event ^ 0xB5297A4DU) % (maximum - minimum + 1);
        }

        template<class Sounds> int update(unsigned int now, unsigned int seed, const Sounds &sounds)
        {
            if(sounds.empty()) return -1;
            if(initialized && now < previous) *this = schedule();
            previous = now;
            if(!initialized)
            {
                next = now;
                advance(seed, sounds);
                initialized = true;
            }
            int result = -1;
            unsigned long long due = 0;
            while(next <= now)
            {
                due = next;
                result = variant;
                advance(seed, sounds);
            }
            // Consume inaudible/missed events without replaying a burst after a
            // stall. The caller decides whether the current event can be heard.
            return result >= 0 && now - due <= 250 ? result : -1;
        }
    };
}

#endif
