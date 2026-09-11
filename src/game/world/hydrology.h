// Included inside namespace game, after baseheight. All caches belong to one worldgenerator/job.
// Planning uses unmodified terrain only, so lookup order cannot affect drainage or recurse into carving.
struct worldhydrology
{
    typedef std::pair<int, int> key;
    enum { STEP = 16, REGION = 256, SOURCEREGION = 128, REACH = 512, TILE = 64, MAXSTEPS = 128 };
    struct node
    {
        float x, y, head;
        key next;
        bool sink;
    };
    struct lake
    {
        float x, y, rx, ry, level, depth, phase, outletx, outlety;
        bool valid, outlet;
        lake() : x(0), y(0), rx(0), ry(0), level(0), depth(0), phase(0), outletx(0), outlety(0), valid(false), outlet(false) {}
        float radius(float px, float py) const
        {
            const float dx = (px - x) / rx, dy = (py - y) / ry, angle = atan2f(dy, dx);
            return sqrtf(dx * dx + dy * dy) / (1.0f + 0.12f * sinf(3 * angle + phase) + 0.07f * sinf(5 * angle - phase));
        }
    };
    struct route
    {
        std::vector<key> nodes;
        lake terminal;
    };
    struct channel
    {
        node a, b;
        float flow, bend;
    };
    struct tile
    {
        std::vector<lake> lakes;
        std::vector<channel> channels;
        worldwatersample columns[TILE * TILE];
        bool ready[TILE * TILE];
        tile() { memset(ready, 0, sizeof(ready)); }
    };

    const worldgenerator &generator;
    FastNoiseLite selector;
    std::map<key, float> heights;
    std::map<key, float> heads;
    std::map<key, node> nodes;
    std::map<key, lake> lakes;
    std::map<key, route> routes;
    std::map<key, tile> tiles;

    worldhydrology(const worldgenerator &generator) : generator(generator)
    {
        setupnoise(selector, generator.seed ^ 0x3E98B617, 0.0017f, 2, 0.35f);
    }

    static int divide(int value, int size)
    {
        return value >= 0 ? value / size : (value + 1) / size - 1;
    }

    uint hash(int x, int y, uint salt = 0) const
    {
        uint h = uint(generator.seed) ^ uint(x) * 0x9E3779B9U ^ uint(y) * 0x85EBCA6BU ^ salt;
        h ^= h >> 16;
        h *= 0x7FEB352DU;
        h ^= h >> 15;
        h *= 0x846CA68BU;
        return h ^ (h >> 16);
    }

    float terrain(int x, int y)
    {
        const key k(x, y);
        std::map<key, float>::iterator found = heights.find(k);
        if(found != heights.end()) return found->second;
        const float value = float(generator.baseheight(x, y));
        heights[k] = value;
        return value;
    }

    float rawhead(const key &k)
    {
        const int x = k.first * STEP, y = k.second * STEP;
        // Low-pass tiny rises before routing; carving follows this shared, strictly descending potential.
        return max(float(generator.settings.sealevel), (terrain(x, y) * 2 + terrain(x - 8, y) + terrain(x + 8, y) +
                   terrain(x, y - 8) + terrain(x, y + 8)) / 6 - 1);
    }

    lake lakeat(float x, float y)
    {
        const key region(divide(int(floorf(x)), REGION), divide(int(floorf(y)), REGION));
        for(int dy = -1; dy <= 1; ++dy) for(int dx = -1; dx <= 1; ++dx)
        {
            const lake l = regionlake(key(region.first + dx, region.second + dy));
            if(l.valid && fabsf(x - l.x) <= l.rx * 1.3f && fabsf(y - l.y) <= l.ry * 1.3f && l.radius(x, y) <= 1) return l;
        }
        return lake();
    }

    float head(const key &k)
    {
        std::map<key, float>::iterator found = heads.find(k);
        if(found != heads.end()) return found->second;
        const lake l = lakeat(float(k.first * STEP), float(k.second * STEP));
        if(!l.valid) return heads[k] = rawhead(k);
        const float dx = k.first * STEP - (l.outlet ? l.outletx : l.x), dy = k.second * STEP - (l.outlet ? l.outlety : l.y);
        // A sub-voxel drainage gradient routes through a flat lake to its single low-side outlet.
        return heads[k] = l.level + sqrtf(dx * dx + dy * dy) * 0.0001f;
    }

    node getnode(const key &k)
    {
        std::map<key, node>::iterator found = nodes.find(k);
        if(found != nodes.end()) return found->second;
        node n;
        const uint h = hash(k.first, k.second);
        n.x = float(k.first * STEP) + (int(h & 7) - 3) * 0.5f;
        n.y = float(k.second * STEP) + (int((h >> 3) & 7) - 3) * 0.5f;
        n.head = head(k);
        n.next = k;
        n.sink = true;
        float best = n.head;
        uint besttie = h;
        for(int dy = -1; dy <= 1; ++dy) for(int dx = -1; dx <= 1; ++dx)
        {
            if(!dx && !dy) continue;
            const key neighbor(k.first + dx, k.second + dy);
            const float candidate = head(neighbor);
            const uint tie = hash(neighbor.first, neighbor.second);
            if(candidate < best || (candidate == best && tie < besttie))
            {
                best = candidate;
                besttie = tie;
                n.next = neighbor;
                n.sink = false;
            }
        }
        nodes[k] = n;
        return n;
    }

    lake makelake(float x, float y, float radius, bool terminal = false)
    {
        lake result;
        const int ix = int(floorf(x)), iy = int(floorf(y));
        const float center = terrain(ix, iy), sea = float(generator.settings.sealevel);
        if(center < sea + 5 || center > sea + 150) return result;
        const uint h = hash(ix, iy, 0xAD51F392U);
        result.x = x;
        result.y = y;
        result.rx = radius;
        result.ry = radius * (0.65f + float(h & 255) / 510.0f);
        result.phase = float((h >> 8) & 255) * (2 * M_PI / 256);
        float low = 1e9f, high = -1e9f;
        int higher = 0;
        loopi(16)
        {
            const float angle = i * (2 * M_PI / 16),
                        rim = terrain(int(x + cosf(angle) * result.rx * 1.4f), int(y + sinf(angle) * result.ry * 1.4f));
            low = min(low, rim);
            high = max(high, rim);
            if(rim >= center) ++higher;
        }
        // Reject peaks, steep hillsides and coastal pools. Basins fit existing low/flat topography.
        if(low < sea + 3 || high - low > max(8.0f, radius * 0.30f) || higher < 10 || low < center - 2) return result;
        result.level = min(center - 1, low - 1);
        if(terminal) result.level = min(result.level, rawhead(key(divide(ix, STEP), divide(iy, STEP))));
        result.depth = min(20.0f, 2.0f + radius * 0.19f + float((h >> 16) & 3));
        const vec position(x * worldclimate::BLOCK_UNITS, y * worldclimate::BLOCK_UNITS,
                           worldclimate::GROUND_UNITS + center * worldclimate::BLOCK_UNITS);
        const float cold = 1.0f - smoothstep(-4.0f, 2.0f, generator.environmentclimate.gettemperature(position)),
                    humidity = generator.environmentclimate.gethumidity(position);
        // Cold basins remain broad and shallow. Dry regions admit fewer isolated ponds.
        if(cold > 0.5f && humidity < 45.0f && (h & 3U)) return result;
        result.depth += cold * (2.0f - result.depth);
        result.valid = true;
        return result;
    }

    lake regionlake(const key &region)
    {
        std::map<key, lake>::iterator found = lakes.find(region);
        if(found != lakes.end()) return found->second;
        lake result;
        const int ox = region.first * REGION, oy = region.second * REGION;
        const float selection = selector.GetNoise(float(ox + REGION / 2), float(oy + REGION / 2));
        if(selection > -0.15f)
        {
            int bx = ox + 128, by = oy + 128;
            float best = 1e9f;
            for(int y = 48; y <= 208; y += 40) for(int x = 48; x <= 208; x += 40)
            {
                const float value = terrain(ox + x, oy + y);
                if(value < best) { best = value; bx = ox + x; by = oy + y; }
            }
            bx = int(floorf(bx / float(STEP) + 0.5f)) * STEP;
            by = int(floorf(by / float(STEP) + 0.5f)) * STEP;
            const uint h = hash(region.first, region.second, 0xB7C1832DU);
            const float radius = (h % 20 == 0) ? 40 + float((h >> 8) % 45) : (h % 4 == 0) ? 12 + float((h >> 8) % 19)
                                                                                                      : 5 + float((h >> 8) % 6);
            result = makelake(float(bx), float(by), radius);
            if(result.valid)
            {
                // The primary outlet shares the drainage graph and exactly the same head as its lake.
                result.level = min(result.level, rawhead(key(divide(bx, STEP), divide(by, STEP))));
                float best = result.level;
                loopi(16)
                {
                    const float angle = i * (2 * M_PI / 16);
                    const key exit(int(floorf((bx + cosf(angle) * (result.rx * 1.4f + STEP)) / STEP + 0.5f)),
                                   int(floorf((by + sinf(angle) * (result.ry * 1.4f + STEP)) / STEP + 0.5f)));
                    const float level = rawhead(exit);
                    if(level < best - 0.25f)
                    {
                        best = level;
                        result.outletx = float(exit.first * STEP);
                        result.outlety = float(exit.second * STEP);
                        result.outlet = true;
                    }
                }
            }
        }
        lakes[region] = result;
        return result;
    }

    const route &regionroute(const key &region)
    {
        std::map<key, route>::iterator found = routes.find(region);
        if(found != routes.end()) return found->second;
        route result;
        const uint h = hash(region.first, region.second, 0x417CB59DU);
        key source(region.first * (SOURCEREGION / STEP) + 2 + int(h % 4),
                   region.second * (SOURCEREGION / STEP) + 2 + int((h >> 8) % 4));
        const lake local = regionlake(key(divide(region.first * SOURCEREGION, REGION), divide(region.second * SOURCEREGION, REGION)));
        const bool outlet = local.valid && local.outlet && divide(int(local.x), SOURCEREGION) == region.first &&
                            divide(int(local.y), SOURCEREGION) == region.second;
        if(outlet) source = key(divide(int(local.x), STEP), divide(int(local.y), STEP));
        const node start = getnode(source);
        const float elevation = start.head - generator.settings.sealevel;
        bool inlandsource = true;
        loopi(8)
        {
            const float angle = i * (2 * M_PI / 8);
            if(terrain(int(start.x + cosf(angle) * 48), int(start.y + sinf(angle) * 48)) <= generator.settings.sealevel)
                inlandsource = false;
        }
        const float slope = max(fabsf(rawhead(key(source.first + 2, source.second)) - rawhead(key(source.first - 2, source.second))),
                                fabsf(rawhead(key(source.first, source.second + 2)) - rawhead(key(source.first, source.second - 2))));
        // Elevated sources are more common. Flat lowlands do not independently sprout rivers.
        if(outlet || (inlandsource && elevation >= 12 && slope > 1 && float((h >> 16) & 255) / 255.0f < min(0.85f, elevation / 80.0f)))
        {
            key current = source;
            bool complete = false;
            loop(step, MAXSTEPS)
            {
                const node n = getnode(current);
                if(abs(n.x - start.x) > REACH || abs(n.y - start.y) > REACH) break;
                result.nodes.push_back(current);
                if(n.head <= generator.settings.sealevel) { complete = true; break; }
                const lake receiving = lakeat(n.x, n.y);
                if(receiving.valid && !receiving.outlet)
                {
                    result.terminal = receiving;
                    complete = true;
                    break;
                }
                if(n.sink)
                {
                    // Every inland endpoint terminates in a topographic basin, never an arbitrary clipped channel.
                    result.terminal = receiving.valid ? receiving : makelake(n.x, n.y, 8 + float(hash(current.first, current.second) % 10), true);
                    complete = result.terminal.valid;
                    break;
                }
                current = n.next;
            }
            if(!complete || result.nodes.size() < 4) { result.nodes.clear(); result.terminal.valid = false; }
        }
        return routes.insert(std::make_pair(region, result)).first->second;
    }

    tile &gettile(const key &position)
    {
        std::map<key, tile>::iterator found = tiles.find(position);
        if(found != tiles.end()) return found->second;
        // Bounded per-job memory. Eviction changes performance only: all plans are pure functions of seed and coordinates.
        if(heights.size() > 180000)
        {
            tiles.clear(); routes.clear(); nodes.clear(); heights.clear(); heads.clear(); lakes.clear();
        }
        // Evict sampled columns without throwing away the much more expensive overlapping drainage plans.
        if(tiles.size() >= 128) tiles.erase(tiles.begin());
        tile &result = tiles[position];
        const int ox = position.first * TILE, oy = position.second * TILE;
        std::map<key, float> flow;
        std::set<key> terminalkeys;
        const int halo = REACH + 160;
        for(int ry = divide(oy - 160, REGION); ry <= divide(oy + TILE + 160, REGION); ++ry)
            for(int rx = divide(ox - 160, REGION); rx <= divide(ox + TILE + 160, REGION); ++rx)
            {
                const key region(rx, ry);
                const lake l = regionlake(region);
                if(l.valid && l.x + l.rx * 1.5f + 32 >= ox && l.x - l.rx * 1.5f - 32 <= ox + TILE &&
                   l.y + l.ry * 1.5f + 32 >= oy && l.y - l.ry * 1.5f - 32 <= oy + TILE) result.lakes.push_back(l);
            }
        for(int ry = divide(oy - halo, SOURCEREGION); ry <= divide(oy + TILE + halo, SOURCEREGION); ++ry)
            for(int rx = divide(ox - halo, SOURCEREGION); rx <= divide(ox + TILE + halo, SOURCEREGION); ++rx)
            {
                const route &r = regionroute(key(rx, ry));
                if(r.terminal.valid && !r.nodes.empty() && terminalkeys.insert(r.nodes.back()).second)
                {
                    const lake &l = r.terminal;
                    if(l.x + l.rx * 1.5f + 32 >= ox && l.x - l.rx * 1.5f - 32 <= ox + TILE &&
                       l.y + l.ry * 1.5f + 32 >= oy && l.y - l.ry * 1.5f - 32 <= oy + TILE) result.lakes.push_back(l);
                }
                for(size_t i = 0; i + 1 < r.nodes.size(); ++i)
                {
                    const node n = getnode(r.nodes[i]);
                    if(n.x < ox - 96 || n.x > ox + TILE + 96 || n.y < oy - 96 || n.y > oy + TILE + 96) continue;
                    flow[r.nodes[i]] += 1;
                }
            }
        for(std::map<key, float>::iterator it = flow.begin(); it != flow.end(); ++it)
        {
            channel c;
            c.a = getnode(it->first);
            c.b = getnode(c.a.next);
            c.flow = it->second;
            c.bend = (float(hash(it->first.first, it->first.second, 37) & 255) / 255 - 0.5f) * 3;
            result.channels.push_back(c);
        }
        return result;
    }

    // Reuse the tile's deterministic drainage plans, never search neighbouring terrain columns.
    // The padded feature lists cover the complete 32-block humidity apron on both sides of a tile boundary.
    float moisture(float x, float y, float z)
    {
        tile &t = gettile(key(divide(int(floorf(x)), TILE), divide(int(floorf(y)), TILE)));
        float influence = 0;
        for(size_t i = 0; i < t.lakes.size(); ++i)
        {
            const lake &l = t.lakes[i];
            const float radius = l.radius(x, y), dx = x - l.x, dy = y - l.y,
                        distance = max(0.0f, sqrtf(dx * dx + dy * dy) * (1.0f - 1.0f / max(radius, 0.0001f))),
                        horizontal = 1.0f - smoothstep(0.0f, 24.0f, distance),
                        vertical = 1.0f - smoothstep(8.0f, 64.0f, fabsf(z - l.level));
            influence = max(influence, horizontal * vertical);
        }
        for(size_t i = 0; i < t.channels.size(); ++i)
        {
            const channel &c = t.channels[i];
            const float vx = c.b.x - c.a.x, vy = c.b.y - c.a.y, length = sqrtf(vx * vx + vy * vy);
            if(length <= 0) continue;
            const float u = clamp(((x - c.a.x) * vx + (y - c.a.y) * vy) / (length * length), 0.0f, 1.0f),
                        bend = sinf(u * M_PI) * c.bend,
                        px = c.a.x + vx * u - vy / length * bend, py = c.a.y + vy * u + vx / length * bend,
                        distance = sqrtf((x - px) * (x - px) + (y - py) * (y - py)),
                        width = min(16.0f, 0.75f + sqrtf(c.flow) * 0.9f) * (1 + 0.12f * sinf((x + y - 1) * 0.035f)),
                        level = c.a.head + (c.b.head - c.a.head) * u,
                        horizontal = 1.0f - smoothstep(width, width + 24.0f, distance),
                        vertical = 1.0f - smoothstep(8.0f, 64.0f, fabsf(z - level));
            influence = max(influence, horizontal * vertical);
        }
        return influence;
    }

    worldwatersample sample(int x, int y)
    {
        const key position(divide(x, TILE), divide(y, TILE));
        const int index = (y - position.second * TILE) * TILE + x - position.first * TILE;
        std::map<key, tile>::iterator cached = tiles.find(position);
        if(cached != tiles.end() && cached->second.ready[index]) return cached->second.columns[index];
        const int sea = generator.settings.sealevel;
        const int base = generator.baseheight(x, y);
        if(base < sea) return worldwatersample(base, sea);
        tile &t = gettile(position);
        worldwatersample result(base, sea);
        float bed = float(base), water = float(sea);
        bool inlake = false;
        for(size_t i = 0; i < t.lakes.size(); ++i)
        {
            const lake &l = t.lakes[i];
            const float r = l.radius(x + 0.5f, y + 0.5f);
            if(r >= 1.35f || base < sea) continue;
            const float blend = 1 - smoothstep(0.75f, 1.35f, r),
                        floor = l.level - 1 - l.depth * max(0.0f, 1 - r * r),
                        target = base + (min(float(base), floor) - base) * blend;
            bed = min(bed, target);
            // Fill every carved column below the waterline, including the blended bank apron.
            if(int(floorf(target + 0.5f)) < int(floorf(l.level + 0.5f)))
            {
                water = max(water, l.level);
                inlake = true;
                result.freshwater = true;
            }
            result.bank = result.bank || (r > 0.82f && r < 1.12f);
        }
        if(!inlake) for(size_t i = 0; i < t.channels.size(); ++i)
        {
            const channel &c = t.channels[i];
            const float vx = c.b.x - c.a.x, vy = c.b.y - c.a.y, length = sqrtf(vx * vx + vy * vy),
                        u = clamp(((x + 0.5f - c.a.x) * vx + (y + 0.5f - c.a.y) * vy) / max(length * length, 1.0f), 0.0f, 1.0f),
                        bend = sinf(u * M_PI) * c.bend,
                        px = c.a.x + vx * u - vy / length * bend, py = c.a.y + vy * u + vx / length * bend,
                        distance = sqrtf((x + 0.5f - px) * (x + 0.5f - px) + (y + 0.5f - py) * (y + 0.5f - py)),
                        size = sqrtf(c.flow), width = min(16.0f, 0.75f + size * 0.9f) * (1 + 0.12f * sinf((x + y) * 0.035f)),
                        depth = min(10.0f, 2.0f + size), level = c.a.head + (c.b.head - c.a.head) * u,
                        bankwidth = width + max(3.0f, depth * 2.5f), r = distance / bankwidth;
            if(r >= 1 || base < sea) continue;
            const float bankblend = 1 - smoothstep(width, bankwidth, distance),
                        slope = fabsf(c.a.head - c.b.head) / max(length, 1.0f),
                        // Leave clearance below the smoothed surface on steep reaches, with a shallow bank apron.
                        clearance = 1.0f + min(4.0f, slope * 1.5f),
                        temperature = generator.environmentclimate.gettemperature(vec(x * float(worldclimate::BLOCK_UNITS),
                            y * float(worldclimate::BLOCK_UNITS), worldclimate::GROUND_UNITS + base * float(worldclimate::BLOCK_UNITS))),
                        cold = 1.0f - smoothstep(-4.0f, 2.0f, temperature),
                        floor = level - clearance - (depth + cold * (2.0f - depth)) * (1 - smoothstep(0.0f, bankwidth, distance)),
                        target = base + (min(float(base), floor) - base) * bankblend,
                        // On a low bank, taper the water down into the excavation instead of ending above dry ground.
                        banklevel = min(level, base - 1 + max(0.0f, level - base + 1) * bankblend);
            bed = min(bed, target);
            if(int(floorf(target + 0.5f)) < int(floorf(banklevel + 0.5f)))
            {
                water = max(water, banklevel);
                result.freshwater = true;
            }
            if(distance > width * 0.8f && distance < width + 1.5f) result.bank = true;
        }
        result.height = max(-255, int(floorf(bed + 0.5f)));
        result.water = int(floorf(water + 0.5f));
        if(result.freshwater) result.height = min(result.height, result.water - 1);
        // Dry bank columns must not acquire a water cap merely from proximity to a feature.
        if(!result.freshwater) result.water = sea;
        t.columns[index] = result;
        t.ready[index] = true;
        return result;
    }
};
