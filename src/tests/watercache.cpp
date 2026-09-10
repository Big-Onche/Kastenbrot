// Build: g++ -O2 -static -Ishared -Iengine -Igame -Ienet/include -Iinclude tests/watercache.cpp -o tests/watercache-check.exe
#include <chrono>
#include "engine.h"
#include "watergeometry.h"
#include "watercache.h"
#undef main

static waterheightcache cache;

int main()
{
    float height = 0;
    const ivec corner(128, 128, 256), distant(8192, 8192, 256);
    assert(!cache.get(corner, height));
    cache.put(corner, 37.5f);
    cache.put(distant, 91.0f);
    assert(cache.get(corner, height) && height == 37.5f);
    // Include both sides of tile boundaries, including the deepest dry-bank and highest water samples.
    const ivec edits[] = { ivec(127, 127, 127), ivec(128, 128, 320), ivec(127, 128, 255), ivec(128, 127, 255) };
    loopi(4)
    {
        cache.put(corner, 37.5f);
        cache.invalidate(edits[i], ivec(edits[i]).add(1));
        assert(!cache.get(corner, height));
        assert(cache.get(distant, height) && height == 91.0f);
    }
    cache.put(corner, 1);
    cache.clear();
    assert(!cache.get(corner, height));
    // A generation wrap must not resurrect entries from an earlier world.
    cache.put(corner, 2);
    cache.serial = ~0U;
    cache.clear();
    assert(!cache.get(corner, height));
    cache.put(corner, 3);
    cache.put(distant, 4);
    cache.invalidate(ivec(0, 0, 0), ivec(65536, 65536, 65536));
    assert(!cache.get(corner, height) && !cache.get(distant, height));

    // Force collisions and eviction, checking values against their full coordinates rather than just the hash.
    loopi(300000) cache.put(ivec(i * 16, (i % 127) * 16, 256), float(i));
    loopi(300000) if(cache.get(ivec(i * 16, (i % 127) * 16, 256), height)) assert(height == float(i));
    cache.clear();

    // Editing a bank must change the cached result just as it changes the uncached sampler.
    int bankheight = 0;
    const auto bankwater = [](int x, int y, int z) { return x < 128 && z >= 0 && z < 48; };
    const auto bankground = [&](int x, int y, int z) { return x >= 128 && z < bankheight; };
    const ivec bankcorner(128, 128, 48);
    cache.put(bankcorner, naturalwatercornerheight(128, 128, 48, bankwater, bankground));
    assert(cache.get(bankcorner, height) && height == 0);
    bankheight = 32;
    cache.invalidate(ivec(128, 127, 0), ivec(129, 129, 32));
    assert(!cache.get(bankcorner, height));
    cache.put(bankcorner, naturalwatercornerheight(128, 128, 48, bankwater, bankground));
    assert(cache.get(bankcorner, height) && height == 32);
    cache.clear();

    const int frames = 40, width = 96;
    int samples = 0, oldsamples = 0;
    const auto water = [&](int x, int y, int z)
    {
        ++samples;
        return z >= 0 && z < 256 + ((x / 64 + y / 128) % 3) * 16;
    };
    const auto solid = [](int x, int y, int z) { return z < 0; };
    double oldsum = 0, newsum = 0;
    hashtable<ivec, float> oldcache(1 << 12);
    const auto start = std::chrono::steady_clock::now();
    loop(frame, frames)
    {
        oldcache.clear();
        loop(pass, 2) loop(y, width) loop(x, width)
        {
            const ivec position(x * 16, y * 16, 256);
            float *cached = oldcache.access(position);
            if(!cached) cached = &oldcache.access(position, naturalwatercornerheight(position.x, position.y, position.z, water, solid));
            oldsum += *cached;
        }
    }
    const auto middle = std::chrono::steady_clock::now();
    oldsamples = samples;
    samples = 0;
    loop(frame, frames) loop(pass, 2) loop(y, width) loop(x, width)
    {
        const ivec position(x * 16, y * 16, 256);
        if(!cache.get(position, height))
        {
            height = naturalwatercornerheight(position.x, position.y, position.z, water, solid);
            cache.put(position, height);
        }
        newsum += height;
    }
    const auto end = std::chrono::steady_clock::now();
    assert(oldsum == newsum);
    assert(samples < oldsamples / 10);
    printf("Passed cache invalidation, world reset, generation wrap, collision and river-height equivalence checks.\n");
    printf("Synthetic %d x %d river, %d frames, two passes: old %.2f ms, cached %.2f ms; terrain samples %d -> %d.\n",
           width, width, frames, std::chrono::duration<double, std::milli>(middle - start).count(),
           std::chrono::duration<double, std::milli>(end - middle).count(), oldsamples, samples);
    return 0;
}
