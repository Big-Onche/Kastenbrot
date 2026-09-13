#ifndef NPCLOOK_H
#define NPCLOOK_H

#include "npcsound.h"

namespace npclook
{
    struct window
    {
        unsigned int serial, seed;
        int elapsed, duration;

        window(unsigned int now, unsigned int identity)
        {
            // Per-NPC phase and per-window jitter avoid a synchronized population.
            const unsigned long long clock = static_cast<unsigned long long>(now) + npcvoice::mix(identity) % 7000U;
            serial = static_cast<unsigned int>(clock / 7000U);
            seed = npcvoice::mix(identity ^ npcvoice::mix(serial) ^ 0xAF3182D7U);
            elapsed = int(clock % 7000U) - int(npcvoice::mix(seed) % 801U);
            duration = 3500 + int(npcvoice::mix(seed ^ 0x63D83595U) % 1501U);
        }

        bool acquiring() const
        {
            // Notice a player entering range during a window, but preserve a substantial glance.
            return elapsed >= 0 && elapsed <= duration - 2200;
        }

        bool active() const
        {
            return elapsed >= 0 && elapsed < duration;
        }
    };

    inline float probability(float distance, float range, float approach)
    {
        if(range <= 0 || distance > range) return 0;
        const float relative = distance / range, proximity = 1 - relative * relative;
        // Approach is radial player speed normalized to a normal walking speed.
        const float chance = 0.10f + 0.89f * proximity;
        return chance + (1 - chance) * 0.70f * approach * proximity;
    }

    inline float roll(unsigned int seed, int clientnum)
    {
        return float(npcvoice::mix(seed ^ npcvoice::mix(static_cast<unsigned int>(clientnum)) ^ 0xB5297A4DU) & 0xFFFFFFU) /
               16777216.0f;
    }
}

#endif
