// Coordinate-only work survives octree moves. Never retain cube or VA pointers
// across frames; resolve ownership again when a tile reaches the renderer.
struct streaminggeometrykey : ivec
{
    streaminggeometrykey(const ivec &origin = ivec(0, 0, 0)) : ivec(origin)
    {
    }
};

static inline uint hthash(const streaminggeometrykey &key)
{
    // Tile-aligned coordinates collide heavily under ivec's x ^ y ^ z hash.
    uint hash = uint(key.x) * 0x9E3779B1U ^ uint(key.y) * 0x85EBCA77U ^ uint(key.z) * 0xC2B2AE3DU;
    hash ^= hash >> 16;
    hash *= 0x7FEB352DU;
    return hash ^ (hash >> 15);
}

struct streaminggeometryqueue
{
    vector<ivec> tiles;
    hashset<streaminggeometrykey> queued;
    hashtable<streaminggeometrykey, int> sections;
    int cursor, mergecursor, mergesize;

    streaminggeometryqueue() : cursor(0), mergecursor(0), mergesize(0)
    {
    }

    void clear()
    {
        tiles.setsize(0);
        queued.clear();
        sections.clear();
        cursor = mergecursor = mergesize = 0;
    }

    int length() const
    {
        return tiles.length() - cursor;
    }

    void add(const ivec &origin, int sectionsize)
    {
        const streaminggeometrykey key(origin);
        if(queued.access(key))
        {
            // A border mount or edit can invalidate preparation of the front
            // tile. Keep its queue position, but redo its merge regions.
            if(tiles[cursor] == origin) mergecursor = 0;
            return;
        }
        queued.add(key);
        tiles.add(origin);
        const streaminggeometrykey section = ivec(origin).mask(~(sectionsize - 1));
        sections.access(section, 0)++;
    }

    bool pending(const ivec &origin)
    {
        return sections.access(streaminggeometrykey(origin)) != NULL;
    }

    void invalidate(const ivec &minimum, const ivec &maximum)
    {
        if(!length()) return;
        const ivec &origin = tiles[cursor];
        loopi(3) if(origin[i] >= maximum[i] + 1 || origin[i] + vatilesize <= minimum[i] - 1) return;
        mergecursor = 0;
    }

    ivec pop(int sectionsize)
    {
        mergecursor = mergesize = 0;
        const streaminggeometrykey origin = tiles[cursor++], section = ivec(origin).mask(~(sectionsize - 1));
        queued.remove(origin);
        int *count = sections.access(section);
        if(count && --*count == 0) sections.remove(section);
        if(cursor == tiles.length()) clear();
        else if(cursor >= 4096 && cursor >= tiles.length() / 2)
        {
            tiles.remove(0, cursor);
            cursor = 0;
        }
        return origin;
    }

    void region(const ivec &minimum, const ivec &maximum, int sectionsize, int worldsize)
    {
        const ivec lo = ivec(minimum).max(0).mask(~(vatilesize - 1)), hi = ivec(maximum).min(worldsize);
        for(int z = lo.z; z < hi.z; z += vatilesize)
            for(int y = lo.y; y < hi.y; y += vatilesize)
                for(int x = lo.x; x < hi.x; x += vatilesize) add(ivec(x, y, z), sectionsize);
    }

    void changed(const ivec &minimum, const ivec &maximum, int sectionsize, int worldsize)
    {
        if(minimum.x >= maximum.x || minimum.y >= maximum.y || minimum.z >= maximum.z) return;
        region(minimum, maximum, sectionsize, worldsize);
        // Visibility depends on face neighbours, not the diagonal sections.
        loopi(3)
        {
            ivec lo(minimum), hi(maximum);
            hi[i] = lo[i];
            --lo[i];
            region(lo, hi, sectionsize, worldsize);
            lo = minimum;
            hi = maximum;
            lo[i] = hi[i]++;
            region(lo, hi, sectionsize, worldsize);
        }
    }
};
