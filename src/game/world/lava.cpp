#include "game.h"
#include "engine.h"
#include "worlddef.h"
#ifdef STANDALONE
#include "worldcube.h"
#endif

bool generateworldlavalakes(worldgencontext &ctx, uchar *carvemap, int chunkx, int chunky)
{
    const int spacing = max(ctx.settings.lavalakespacing, 1), verticalspacing = max(spacing / 2, 8),
              minradius = min(ctx.settings.lavalakeminsize, ctx.settings.lavalakemaxsize),
              maxradius = max(ctx.settings.lavalakeminsize, ctx.settings.lavalakemaxsize),
              bottomlayers = clamp(ctx.settings.bottomlavalayers, 0, int(WORLD_HEIGHT_BLOCKS)), minimumheight = WORLD_MIN_HEIGHT + bottomlayers,
              startheight = max(ctx.settings.lavalakestartheight, ctx.settings.lavalakedeepheight),
              deepheight = min(ctx.settings.lavalakestartheight, ctx.settings.lavalakedeepheight);
    const long long chunkstartx = (long long)chunkx * WORLD_CHUNK_BLOCKS, chunkstarty = (long long)chunky * WORLD_CHUNK_BLOCKS,
                    mincellx = worldfloordiv(chunkstartx - maxradius, spacing),
                    maxcellx = worldfloordiv(chunkstartx + WORLD_CHUNK_BLOCKS - 1 + maxradius, spacing),
                    mincelly = worldfloordiv(chunkstarty - maxradius, spacing),
                    maxcelly = worldfloordiv(chunkstarty + WORLD_CHUNK_BLOCKS - 1 + maxradius, spacing);
    const int mincellz = int(worldfloordiv(minimumheight - maxradius, verticalspacing)), maxcellz = int(worldfloordiv(startheight, verticalspacing));

    for(long long celly = mincelly; celly <= maxcelly; ++celly)
        for(long long cellx = mincellx; cellx <= maxcellx; ++cellx)
            for(int cellz = mincellz; cellz <= maxcellz; ++cellz)
            {
                if(ctx.iscanceled()) return false;
                const uint positionhash = hashworldfeature(uint(ctx.seed), cellx, celly, cellz, 0xC13FA9A9U),
                           chancehash = hashworldfeature(uint(ctx.seed), cellx, celly, cellz, 0x91E10DA5U),
                           sizehash = hashworldfeature(uint(ctx.seed), cellx, celly, cellz, 0xD192ED03U);
                const long long centerx = cellx * spacing + int(positionhash % uint(spacing)),
                                centery = celly * spacing + int((positionhash >> 8) % uint(spacing));
                const int centerz = cellz * verticalspacing + int((positionhash >> 16) % uint(verticalspacing));
                if(centerz < minimumheight || centerz > startheight) continue;

                const float approachweight =
                                deepheight < startheight ? clamp((startheight - centerz) / float(startheight - deepheight), 0.0f, 1.0f) : 1.0f,
                            deepweight = deepheight > minimumheight ? clamp((deepheight - centerz) / float(deepheight - minimumheight), 0.0f, 1.0f)
                                         : centerz <= deepheight    ? 1.0f
                                                                    : 0.0f,
                            lakechance = ctx.settings.lavalakeshallowchance * approachweight +
                                         (ctx.settings.lavalakedeepchance - ctx.settings.lavalakeshallowchance) * deepweight;
                if(worldtreeunit(chancehash) >= clamp(lakechance, 0.0f, 1.0f)) continue;

                const int depthmaxradius =
                              clamp(int(floor(minradius + (maxradius - minradius) * (0.25f + deepweight * 0.75f) + 0.5f)), minradius, maxradius),
                          radiusrange = max(depthmaxradius - minradius + 1, 1), radius = minradius + int(sizehash % uint(radiusrange)),
                          minorradius = max(2, int(floor(radius * (0.55f + ((sizehash >> 8) & 0xFFU) / 637.5f) + 0.5f))),
                          verticalradius = max(2, (radius + minorradius) / 4),
                          lavalevel = centerz - int((sizehash >> 28) % uint(max(verticalradius / 2, 1)));
                const float angle = ((sizehash >> 16) & 0x0FFFU) / 4096.0f * 2.0f * M_PI, anglecos = cosf(angle), anglesin = sinf(angle),
                            lobeangle = angle + (((positionhash >> 24) & 0xFFU) / 255.0f - 0.5f) * M_PI,
                            lobedistance = radius * (0.15f + ((chancehash >> 24) & 0xFFU) / 1275.0f), lobecenterx = cosf(lobeangle) * lobedistance,
                            lobecentery = sinf(lobeangle) * lobedistance, loberadius = max(radius * 0.62f, 1.0f),
                            lobeminorradius = max(minorradius * 0.7f, 1.0f), shapevariation = clamp(ctx.settings.lavalakeshapevariation, 0.0f, 0.75f);
                const int centerlocalx = int(centerx - chunkstartx), centerlocaly = int(centery - chunkstarty);
                if(centerlocalx + radius < 0 || centerlocalx - radius >= WORLD_CHUNK_BLOCKS || centerlocaly + radius < 0 ||
                   centerlocaly - radius >= WORLD_CHUNK_BLOCKS)
                    continue;

                const int centerblockx = int(centerx - (long long)chunkx * WORLD_CHUNK_BLOCKS),
                          centerblocky = int(centery - (long long)chunky * WORLD_CHUNK_BLOCKS),
                          centerheight = generateworldheight(ctx, chunkx, chunky, centerblockx, centerblocky) / WORLD_BLOCK_SIZE;
                if(centerz + verticalradius > centerheight - ctx.settings.cavemindepth) continue;

                const int xmin = max(centerlocalx - radius, 0), xmax = min(centerlocalx + radius, WORLD_CHUNK_BLOCKS - 1),
                          ymin = max(centerlocaly - radius, 0), ymax = min(centerlocaly + radius, WORLD_CHUNK_BLOCKS - 1),
                          zmin = max(centerz - verticalradius, minimumheight), zmax = min(centerz + verticalradius, WORLD_MAX_HEIGHT - 1);
                for(int y = ymin; y <= ymax; ++y)
                    for(int x = xmin; x <= xmax; ++x)
                    {
                        const float localx = float(x - centerlocalx), localy = float(y - centerlocaly),
                                    rotatedx = localx * anglecos + localy * anglesin, rotatedy = -localx * anglesin + localy * anglecos,
                                    primary = rotatedx * rotatedx / float(radius * radius) + rotatedy * rotatedy / float(minorradius * minorradius),
                                    lobex = rotatedx - lobecenterx, lobey = rotatedy - lobecentery,
                                    lobe = lobex * lobex / (loberadius * loberadius) + lobey * lobey / (lobeminorradius * lobeminorradius),
                                    horizontal = min(primary, lobe),
                                    shapenoise = ctx.generator.lakeshape.GetNoise(float(chunkx) * WORLD_CHUNK_BLOCKS + x + 9200.5f,
                                                                                  float(chunky) * WORLD_CHUNK_BLOCKS + y - 9200.5f),
                                    boundary = 1.0f - shapevariation * 0.5f + shapenoise * shapevariation * 0.5f;
                        if(horizontal > boundary) continue;

                        const int surfaceheight = ctx.heightmap[y * WORLD_CHUNK_BLOCKS + x] / WORLD_BLOCK_SIZE;
                        for(int logicalz = zmin; logicalz <= zmax; ++logicalz)
                        {
                            if(surfaceheight - logicalz < ctx.settings.cavemindepth) continue;
                            const float dz = (logicalz - centerz) / float(verticalradius);
                            if(horizontal + dz * dz > boundary) continue;

                            uchar &carve = carvemap[worldcarveindex(x, y, logicalz - WORLD_MIN_HEIGHT)];
                            if(logicalz <= lavalevel)
                                carve = WORLD_CARVE_LAVA | (carve & WORLD_CARVE_ENTRANCE);
                            else if(!(carve & WORLD_CARVE_TYPE))
                                carve = WORLD_CARVE_AIR | (carve & WORLD_CARVE_ENTRANCE);
                        }
                    }
            }
    return true;
}
