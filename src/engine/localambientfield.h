#ifndef LOCALAMBIENTFIELD_H
#define LOCALAMBIENTFIELD_H

#include <algorithm>
#include <vector>

// Pure snapshot solver: no world, renderer, or GL access. Unknown cells (128)
// transmit daylight; only captured geometry (255) blocks it.
namespace ambientfield
{
    struct color
    {
        unsigned char sky, r, g, b;
        color() : sky(0), r(0), g(0), b(0) {}
    };

    inline int resolution(int requested, int distance, int maximum)
    {
        // Grow cell spacing instead of silently truncating the requested extent.
        return std::max(requested, (2 * distance + maximum - 1) / maximum);
    }

    struct field
    {
        int x, y, z, loss, downloss, gipasses;
        float decay;
        std::vector<unsigned char> solid, sky;
        std::vector<color> albedo, light;

        field(int x, int y, int z) : x(x), y(y), z(z), loss(4), downloss(4), gipasses(6), decay(0.8f) {}

        int neighbours(int i, int *out) const
        {
            int n = 0, px = i % x, py = (i / x) % y, pz = i / (x * y);
            if(px) out[n++] = i - 1;
            if(px + 1 < x) out[n++] = i + 1;
            if(py) out[n++] = i - x;
            if(py + 1 < y) out[n++] = i + x;
            if(pz) out[n++] = i - x * y;
            if(pz + 1 < z) out[n++] = i + x * y;
            return n;
        }

        template<class Cancel> bool solve(Cancel cancelled)
        {
            const int count = x * y * z, plane = x * y;
            light.assign(count, color());
            std::vector<int> buckets[256];
            // Seed entire open sky columns, including clearance above the volume.
            for(int column = 0; column < plane; ++column)
            {
                if((column & 255) == 0 && cancelled()) return false;
                bool visible = sky[column] != 0;
                for(int pz = z - 1; pz >= 0; --pz)
                {
                    const int i = pz * plane + column;
                    if(solid[i] == 255) visible = false;
                    else if(visible || solid[i] == 128) light[i].sky = 255;
                }
            }
            // Only the boundary of seeded air needs to enter the wavefront.
            for(int i = 0; i < count; ++i)
            {
                if((i & 4095) == 0 && cancelled()) return false;
                if(!light[i].sky) continue;
                int adjacent[6], n = neighbours(i, adjacent);
                for(int j = 0; j < n; ++j)
                    if(solid[adjacent[j]] != 255 && !light[adjacent[j]].sky)
                    {
                        buckets[255].push_back(i);
                        break;
                    }
            }
            // Descending integer buckets implement a multi-source shortest-path
            // solve. Each settled air cell visits its neighbours once, rather
            // than scanning the entire volume up to 64 times on the GPU.
            for(int value = 255; value > 0; --value)
            {
                std::vector<int> &bucket = buckets[value];
                for(size_t k = 0; k < bucket.size(); ++k)
                {
                    if((k & 4095) == 0 && cancelled()) return false;
                    const int i = bucket[k];
                    if(light[i].sky != value) continue;
                    int adjacent[6], n = neighbours(i, adjacent);
                    for(int j = 0; j < n; ++j)
                    {
                        const int to = adjacent[j], next = value - (to == i - plane ? downloss : loss);
                        if(solid[to] == 255 || next <= light[to].sky) continue;
                        light[to].sky = static_cast<unsigned char>(next);
                        buckets[next].push_back(to);
                    }
                }
                std::vector<int>().swap(bucket);
            }
            if(gipasses < 0) return !cancelled();

            struct entry { int index; color value; };
            std::vector<entry> frontier, nextfrontier;
            for(int i = 0; i < count; ++i)
            {
                if((i & 4095) == 0 && cancelled()) return false;
                if(solid[i] == 255 || !light[i].sky) continue;
                int adjacent[6], n = neighbours(i, adjacent);
                color source;
                for(int j = 0; j < n; ++j) if(solid[adjacent[j]] == 255)
                {
                    const color &surface = albedo[adjacent[j]];
                    source.r = std::max(source.r, surface.r);
                    source.g = std::max(source.g, surface.g);
                    source.b = std::max(source.b, surface.b);
                }
                color &out = light[i];
                out.r = (int(source.r) * out.sky + 127) / 255;
                out.g = (int(source.g) * out.sky + 127) / 255;
                out.b = (int(source.b) * out.sky + 127) / 255;
                if(out.r || out.g || out.b) frontier.push_back(entry{i, out});
            }
            // Sparse, synchronous bounce propagation preserves the old max-RGB
            // diffusion and pass count. Snapshot each frontier before advancing.
            std::vector<int> marked(count, -1);
            for(int pass = 0; pass < gipasses && !frontier.empty(); ++pass)
            {
                nextfrontier.clear();
                for(size_t k = 0; k < frontier.size(); ++k)
                {
                    if((k & 4095) == 0 && cancelled()) return false;
                    const entry &from = frontier[k];
                    const unsigned char r = static_cast<unsigned char>(from.value.r * decay + 0.5f),
                                        g = static_cast<unsigned char>(from.value.g * decay + 0.5f),
                                        b = static_cast<unsigned char>(from.value.b * decay + 0.5f);
                    int adjacent[6], n = neighbours(from.index, adjacent);
                    for(int j = 0; j < n; ++j)
                    {
                        const int to = adjacent[j];
                        color &out = light[to];
                        if(solid[to] == 255 || (r <= out.r && g <= out.g && b <= out.b)) continue;
                        out.r = std::max(out.r, r);
                        out.g = std::max(out.g, g);
                        out.b = std::max(out.b, b);
                        if(marked[to] == pass) continue;
                        marked[to] = pass;
                        nextfrontier.push_back(entry{to, color()});
                    }
                }
                for(size_t k = 0; k < nextfrontier.size(); ++k) nextfrontier[k].value = light[nextfrontier[k].index];
                frontier.swap(nextfrontier);
            }
            return !cancelled();
        }
    };
}

#endif
