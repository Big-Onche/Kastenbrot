// Coordinate-only work survives octree moves. Never retain cube or VA pointers
// across frames; resolve ownership again when a tile reaches the renderer.
struct streaminggeometryqueue
{
    vector<ivec> tiles;
    hashset<ivec> queued;
    hashtable<ivec, int> sections;
    int cursor;

    streaminggeometryqueue() : cursor(0)
    {
    }

    void clear()
    {
        tiles.setsize(0);
        queued.clear();
        sections.clear();
        cursor = 0;
    }

    int length() const
    {
        return tiles.length() - cursor;
    }

    void add(const ivec &origin, int sectionsize)
    {
        if(queued.access(origin)) return;
        queued.add(origin);
        tiles.add(origin);
        const ivec section = ivec(origin).mask(~(sectionsize - 1));
        sections.access(section, 0)++;
    }

    bool pending(const ivec &origin)
    {
        return sections.access(origin) != NULL;
    }

    ivec pop(int sectionsize)
    {
        const ivec origin = tiles[cursor++], section = ivec(origin).mask(~(sectionsize - 1));
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
        const ivec lo = ivec(minimum).max(0).mask(~(WORLD_VA_TILE_SIZE - 1)), hi = ivec(maximum).min(worldsize);
        for(int z = lo.z; z < hi.z; z += WORLD_VA_TILE_SIZE)
            for(int y = lo.y; y < hi.y; y += WORLD_VA_TILE_SIZE)
                for(int x = lo.x; x < hi.x; x += WORLD_VA_TILE_SIZE) add(ivec(x, y, z), sectionsize);
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
