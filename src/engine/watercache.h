#ifndef __WATER_CACHE_H__
#define __WATER_CACHE_H__

// Mix all coordinate bits: x ^ y ^ z puts voxel-aligned river corners into very few buckets.
static inline uint watergeometryhash(const ivec &position)
{
    uint hash = uint(position.x) * 0x8DA6B343U ^ uint(position.y) * 0xD8163841U ^ uint(position.z) * 0xCB1AB31FU;
    hash ^= hash >> 16;
    hash *= 0x7FEB352DU;
    return hash ^ (hash >> 15);
}

// Bounded, four-way cache of static corner heights. Region generations let an edit invalidate its neighbours
// without scanning every cached corner. Hash collisions only discard extra work; positions are always checked.
struct waterheightcache
{
    enum { BUCKETS = 1 << 15, WAYS = 4, REGIONS = 1 << 12, REGION_SHIFT = 7 };
    struct entry
    {
        ivec position;
        float height;
        uint version;

        entry() : position(0, 0, 0), height(0), version(0) {}
    };
    entry entries[BUCKETS][WAYS];
    uint regions[REGIONS], serial, replacement;

    waterheightcache() : serial(0), replacement(0)
    {
        clear();
    }

    static uint region(const ivec &position)
    {
        return watergeometryhash(ivec(position).shr(REGION_SHIFT)) & (REGIONS - 1);
    }

    uint nextversion()
    {
        if(!++serial)
        {
            loopi(BUCKETS) loopj(WAYS) entries[i][j].version = 0;
            serial = 1;
        }
        return serial;
    }

    void clear()
    {
        const uint version = nextversion();
        loopi(REGIONS) regions[i] = version;
    }

    void invalidate(const ivec &minimum, const ivec &maximum)
    {
        // The corner sampler reads x/y - 1, water up to z + 64, and bank ground down to z - 129.
        const ivec first = ivec(minimum).sub(ivec(1, 1, 64)).shr(REGION_SHIFT),
                   last = ivec(maximum).add(ivec(1, 1, 129)).shr(REGION_SHIFT);
        if((long long)(last.x - first.x + 1) * (last.y - first.y + 1) * (last.z - first.z + 1) >= REGIONS)
        {
            clear();
            return;
        }
        const uint version = nextversion();
        for(int z = first.z; z <= last.z; ++z)
        for(int y = first.y; y <= last.y; ++y)
        for(int x = first.x; x <= last.x; ++x)
            regions[watergeometryhash(ivec(x, y, z)) & (REGIONS - 1)] = version;
    }

    bool get(const ivec &position, float &height) const
    {
        const entry *bucket = entries[watergeometryhash(position) & (BUCKETS - 1)];
        const uint version = regions[region(position)];
        loopi(WAYS) if(bucket[i].version == version && bucket[i].position == position)
        {
            height = bucket[i].height;
            return true;
        }
        return false;
    }

    void put(const ivec &position, float height)
    {
        entry *bucket = entries[watergeometryhash(position) & (BUCKETS - 1)], *target = NULL;
        loopi(WAYS)
        {
            if(bucket[i].position == position)
            {
                target = &bucket[i];
                break;
            }
            if(!target && bucket[i].version != regions[region(bucket[i].position)]) target = &bucket[i];
        }
        if(!target) target = &bucket[replacement++ % WAYS];
        target->position = position;
        target->height = height;
        target->version = regions[region(position)];
    }
};

#endif
