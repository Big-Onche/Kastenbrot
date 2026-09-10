#ifndef GAME_NPCSPAWN_H
#define GAME_NPCSPAWN_H

// Cave groups share a world-seeded stream, independent of the selected species.
static inline uint cavepoolseed(int worldseed, int cellx, int celly, int band, int bands)
{
    return worlddrophash(uint(worldseed) ^ uint(cellx) * 0x9E3779B9U ^ uint(celly) * 0x85EBCA6BU ^
                        worlddrophash(uint(band) ^ 0xB5297A4DU) ^ worlddrophash(uint(bands) ^ 0x43505632U));
}

static inline ullong cavepoolspawnkey(int worldseed, int cellx, int celly, int band, int bands, int member)
{
    ullong hash = 1469598103934665603ULL;
    const uint values[] = { 0x43505632U, uint(worldseed), uint(cellx), uint(celly), uint(band), uint(bands), uint(member) };
    loopi(7) loopj(4)
    {
        hash ^= uchar(values[i] >> (j * 8));
        hash *= 1099511628211ULL;
    }
    return hash ? hash : 1;
}

struct cavenpcpool
{
    vector<const npcdefinition *> definitions;

    void sort()
    {
        // Canonical order makes weighting and placement independent of registration order.
        definitions.sort([](const npcdefinition *a, const npcdefinition *b) { return cubecasecmp(a->id, b->id) < 0; });
    }

    const npcdefinition *choose(int worldseed, int cellx, int celly, int band, int bands) const
    {
        if(band < 0 || band >= bands || bands <= 0) return NULL;
        double total = 0;
        loopv(definitions)
        {
            const npcdefinition &definition = *definitions[i];
            if(definition.attitude == NPC_AGGRESSIVE && definition.cavebands == bands && definition.weight > 0)
                total += definition.weight;
        }
        if(total <= 0) return NULL;
        const uint roll = worlddrophash(cavepoolseed(worldseed, cellx, celly, band, bands) ^ 0xD1B54A35U);
        double choice = (double(roll) / 4294967296.0) * total;
        const npcdefinition *last = NULL;
        loopv(definitions)
        {
            const npcdefinition &definition = *definitions[i];
            if(definition.attitude != NPC_AGGRESSIVE || definition.cavebands != bands || definition.weight <= 0) continue;
            last = &definition;
            if(choice < definition.weight) return last;
            choice -= definition.weight;
        }
        return last;
    }
};

#endif
