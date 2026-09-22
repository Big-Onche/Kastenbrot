#include "game.h"
#include "engine.h"
#include "worlddef.h"
#ifdef STANDALONE
#include "worldcube.h"
#endif

static bool dryworldspawnblock(const game::worldgenerator &generator, const game::worldsettings &settings, int x, int y)
{
    const int height = generator.height(x, y);
    return height >= generator.surface(x, y).water && height <= WORLD_MAX_HEIGHT - 3;
}

bool game::chooseworldspawn(double originx, double originy, double &spawnx, double &spawny)
{
    const int originblockx = int(floor(originx / WORLD_BLOCK_SIZE)), originblocky = int(floor(originy / WORLD_BLOCK_SIZE));

    game::worldsettings settings;
    game::worldgenerator generator(game::getworldseed(), settings);

    if(dryworldspawnblock(generator, settings, originblockx, originblocky))
    {
        spawnx = (double(originblockx) + 0.5) * WORLD_BLOCK_SIZE;
        spawny = (double(originblocky) + 0.5) * WORLD_BLOCK_SIZE;
        return true;
    }

#ifndef STANDALONE
    renderprogress(0.82f, "choosing a better spawn point because you had no chance...");
#endif

    int bestx = originblockx, besty = originblocky;
    long long bestdist = LLONG_MAX;

    // Search every nearby block first, then cover a continent-scale area on a
    // coarse grid. A final local pass turns the best coarse hit into a block-
    // precise dry spawn without evaluating millions of noise samples.
    const int exactradius = 64;
    for(int y = originblocky - exactradius; y <= originblocky + exactradius; ++y)
        for(int x = originblockx - exactradius; x <= originblockx + exactradius; ++x)
        {
            const long long dx = x - originblockx, dy = y - originblocky, dist = dx * dx + dy * dy;
            if(dist >= bestdist) continue;
            if(!dryworldspawnblock(generator, settings, x, y)) continue;
            bestx = x;
            besty = y;
            bestdist = dist;
        }

    if(bestdist == LLONG_MAX)
    {
        const int searchradius = 8192, searchstep = 64;
        for(int y = originblocky - searchradius; y <= originblocky + searchradius; y += searchstep)
            for(int x = originblockx - searchradius; x <= originblockx + searchradius; x += searchstep)
            {
                const long long dx = x - originblockx, dy = y - originblocky, dist = dx * dx + dy * dy;
                if(dist >= bestdist) continue;
                if(!dryworldspawnblock(generator, settings, x, y)) continue;
                bestx = x;
                besty = y;
                bestdist = dist;
            }
    }

    if(bestdist == LLONG_MAX) return false;

    {
        const int refine = 64;
        int refinedx = bestx, refinedy = besty;
        long long refineddist = bestdist;
        for(int y = besty - refine; y <= besty + refine; ++y)
            for(int x = bestx - refine; x <= bestx + refine; ++x)
            {
                const long long dx = x - originblockx, dy = y - originblocky, dist = dx * dx + dy * dy;
                if(dist >= refineddist) continue;
                if(!dryworldspawnblock(generator, settings, x, y)) continue;
                refinedx = x;
                refinedy = y;
                refineddist = dist;
            }
        bestx = refinedx;
        besty = refinedy;
    }

    spawnx = (double(bestx) + 0.5) * WORLD_BLOCK_SIZE;
    spawny = (double(besty) + 0.5) * WORLD_BLOCK_SIZE;
    return true;
}
