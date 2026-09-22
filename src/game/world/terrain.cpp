#include "game.h"

namespace game
{
    static float grassplateauweight(float geology)
    {
        return smoothstep(0.05f, 0.45f, -geology);
    }

    static float hillrockweight(float detail, float micro)
    {
        // Small summit outcrops and a few recessed flank exposures, each in coherent multi-block patches.
        return max(smoothstep(0.35f, 0.60f, micro) * smoothstep(0.05f, 0.40f, detail),
                   0.65f * smoothstep(0.40f, 0.65f, -micro) * smoothstep(0.10f, 0.40f, -detail));
    }

    static float grassplateauheight(float height, float weight, float macro, float detail)
    {
        if(height <= 100.0f || height >= 150.0f || weight <= 0.0f) return height;
        // Regional height and local rolls break up identical shelves. Every shoulder still ends inside 100-150.
        const float variation = smoothstep(0.20f, 0.80f, macro), center = 110.0f + 30.0f * variation + (2.0f + 2.0f * variation) * detail,
                    offset = height - center, span = offset < 0.0f ? center - 100.0f : 150.0f - center,
                    width = 0.06f + 0.32f * smoothstep(-0.55f, 0.55f, detail), t = clamp((fabsf(offset) / span - width) / (1.0f - width), 0.0f, 1.0f),
                    shoulder = span * t * t * ((2.0f + width) - (1.0f + width) * t), target = center + (offset < 0.0f ? -shoulder : shoulder);
        // Retain at least a quarter of the original slope and microrelief, even in the strongest meadow patches.
        return height + 0.75f * weight * (target - height);
    }

    static float landthreshold(const worldsettings &settings)
    {
        const float coverage = settings.oceancoverage + settings.terraincoverage;
        const float oceanratio = coverage > 0.0f ? settings.oceancoverage / coverage : 0.5f;

        return oceanratio <= 0.0f ? -0.98f : oceanratio >= 1.0f ? 0.98f : oceanratio - 0.5f;
    }

    float samplecontinental(const worldgenerator &generator, float noisex, float noisey)
    {
        const float threshold = landthreshold(generator.settings), mega = generator.geology.GetNoise(noisex, noisey),
                    macro = generator.covenoise.GetNoise(noisex, noisey),
                    regionalbias = generator.oceanregional.GetNoise(noisex, noisey) * generator.settings.oceanregionalbias,
                    broad = 0.82f * mega + 0.24f * macro - regionalbias, detailstrength = generator.settings.coastdetailstrength,
                    detailband = max(detailstrength * 3.5f, 0.10f),
                    detailweight = 1.0f - smoothstep(detailstrength * 0.35f, detailband, fabs(broad - threshold));

        if(detailstrength <= 0.0f || detailweight <= 0.0f) return broad;
        return broad + generator.coastdetail.GetNoise(noisex, noisey) * detailstrength * detailweight;
    }

    static float sampleterrainmicrovariation(const worldgenerator &generator, float noisex, float noisey)
    {
        const float detail = generator.terrainmicro.GetNoise(noisex, noisey),
                    masknoise = clamp(generator.terrainmicromask.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                    mask = 0.45f + 0.55f * smoothstep(0.25f, 0.75f, masknoise);

        return detail * mask;
    }

    static void samplecoastprofile(const worldgenerator &generator, float noisex, float noisey, float &beachspan, float &plainrun, float &plainlevel)
    {
        const float beachshape = clamp(generator.beachnoise.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                    grassshape = clamp(generator.coastshape.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                    configuredspan = max(float(generator.settings.coastwidth), 1.0f);

        // Each of the two sand terraces spans roughly 40-120% of the configured
        // coast width. This produces broad natural beaches without changing
        // their fixed vertical sequence at sea level and sea level +1.
        beachspan = configuredspan * (0.40f + 0.80f * powf(beachshape, 2.2f));
        plainrun = 14.0f + 16.0f * (1.0f - powf(beachshape, 1.5f));
        plainlevel = 2.0f + grassshape;
    }

    static float samplecliffstrength(const worldgenerator &generator, float noisex, float noisey)
    {
        const float selector = clamp(generator.cliffnoise.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                    chance = clamp(generator.settings.cliffchance * 0.01f, 0.0f, 1.0f), center = 1.0f - chance;

        if(chance <= 0.0f) return 0.0f;
        if(chance >= 1.0f) return 1.0f;

        // Cliffs occur in coherent coastal sections, not as per-column accidents.
        // An eight-percent feather on each side keeps their boundaries gradual.
        return smoothstep(center - 0.08f, center + 0.08f, selector);
    }

    float samplecoastdistance(const worldgenerator &generator, float noisex, float noisey, float continental)
    {
        const float gradientstep = 24.0f,
                    gradientx =
                        (samplecontinental(generator, noisex + gradientstep, noisey) - samplecontinental(generator, noisex - gradientstep, noisey)) /
                        (2.0f * gradientstep),
                    gradienty =
                        (samplecontinental(generator, noisex, noisey + gradientstep) - samplecontinental(generator, noisex, noisey - gradientstep)) /
                        (2.0f * gradientstep),
                    minimumgradient = generator.settings.macrocontinentfrequency * 0.35f,
                    maximumgradient = generator.settings.macrocontinentfrequency * 0.85f,
                    gradient = clamp(sqrtf(gradientx * gradientx + gradienty * gradienty), minimumgradient, maximumgradient);

        // A broad derivative follows the coast's overall normal without letting
        // individual coast-detail octaves reset an already-inland point back
        // into either sea-level sand terrace.
        return max((continental - landthreshold(generator.settings)) / max(gradient, 0.000001f), 0.0f);
    }

    static worldtectonicsample sampletectonics(const worldgenerator &generator, int x, int y, float continental, float cavedepth)
    {
        const worldsettings &settings = generator.settings;
        const float threshold = landthreshold(settings), landdensity = continental - threshold,
                    protection = max(0.02f, settings.coastprotectionwidth * settings.macrocontinentfrequency * 0.75f);

        float tectonicx = x + 10000.5f, tectonicy = y - 10000.5f;
        generator.tectonicwarp.DomainWarp(tectonicx, tectonicy);

        // Stretch the chain spacing, then partially compensate the profile to retain typical ridge widths.
        // Full linear compensation narrows the shoulders of the curved noise belts too much.
        // Local summit/shoulder frequencies stay unchanged; spacing does not shrink the individual mountains.
        const float ridgewidth = powf(settings.mountainspacing, 0.75f),
                    ridge = powf(clamp(1.0f - ridgewidth * fabs(generator.tectonicnoise.GetNoise(tectonicx, tectonicy)), 0.0f, 1.0f),
                                 max(settings.tectonicridgepower * 0.65f, 0.1f)),
                    noisex = x + 10000.5f, noisey = y - 10000.5f,
                    broadchain = clamp(generator.mountainrange.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                    chainstrength = clamp(0.78f * ridge + 0.22f * broadchain, 0.0f, 1.0f),
                    localgeology = generator.foothillgeology.GetNoise(noisex, noisey),
                    geologydetail = generator.foothillgeology.GetNoise(noisex * 2.7f + 1731.0f, noisey * 2.7f - 2917.0f);

        worldtectonicsample sample;
        sample.activity =
            smoothstep(settings.tectonicactivitythreshold, min(settings.tectonicactivitythreshold + 0.35f, 1.0f), max(ridge, chainstrength));

        const float oceandistance = clamp(-landdensity / max(threshold + 1.0f, 0.001f), 0.0f, 1.0f),
                    oceanshelf = smoothstep(0.0f, 0.25f, oceandistance), deepocean = smoothstep(0.15f, 0.85f, oceandistance),
                    normaloceandepth = settings.maxoceandepth * (0.25f * oceanshelf + 0.75f * deepocean),

                    // Give tall relief enough inland distance to fade before the
                    // protected coast instead of clipping a mountain into a wall.
            landmask = smoothstep(protection, protection + 0.40f, landdensity),
                    oceandensitymask = smoothstep(protection, protection + 0.16f, -landdensity),
                    deepoceanmask = oceandensitymask * smoothstep(40.0f, 100.0f, normaloceandepth),
                    hill = clamp(generator.hills.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                    reliefcoverage =
                        max(settings.plainscoverage + settings.hillscoverage + settings.mountainscoverage + settings.highsummitscoverage, 0.001f),
                    mountainshare = (settings.mountainscoverage + settings.highsummitscoverage) / reliefcoverage,
                    configuredthreshold = clamp(settings.mountainthreshold + (0.23f - mountainshare) * 0.45f, 0.08f, 0.92f),
                    envelopewidth = max(settings.mountainwidth, 0.01f),
                    // Expand the foot of the existing chains without moving their crests or changing the noise frequencies.
            reliefscale = max(settings.mountainmaxamplitude / 160.0f, 1.0f), footwidth = envelopewidth * sqrtf(reliefscale),
                    hillregion = smoothstep(configuredthreshold - footwidth * 2.4f, configuredthreshold - envelopewidth * 0.25f, chainstrength),
                    mountainregion = smoothstep(configuredthreshold - footwidth, configuredthreshold + envelopewidth, chainstrength),
                    summitregion =
                        smoothstep(configuredthreshold + envelopewidth * 0.55f, configuredthreshold + envelopewidth * 1.75f, chainstrength),
                    // Broader shoulders compensate for the extra height instead of stretching narrow peaks vertically.
            primaryridge = powf(clamp(1.0f - fabs(generator.mountainnoise.GetNoise(noisex, noisey)), 0.0f, 1.0f), 1.55f / reliefscale),
                    secondaryridge = powf(clamp(1.0f - fabs(generator.mountainpeaks.GetNoise(noisex, noisey)), 0.0f, 1.0f), 1.85f / reliefscale),
                    plainhillshape = smoothstep(0.48f, 0.78f, hill), backgroundrelief = 0.025f * plainhillshape * (1.0f - 0.75f * hillregion),
                    foothills = 0.14f * hillregion * (0.45f + 0.55f * hill),
                    mainridges = 0.52f * powf(mountainregion, 1.55f) * (0.24f + 0.76f * primaryridge),
                    surroundingpeaks = 0.24f * powf(mountainregion, 2.0f) * secondaryridge * (0.30f + 0.70f * primaryridge),
                    localsummits = 0.19f * powf(summitregion, 2.4f) * powf(primaryridge * secondaryridge, 1.15f),
                    // Positive patches grow low satellite summits around the belts, fading before their main crests.
                    // Other patches add no relief, retaining direct transitions from plains into mountains.
            hillpatch = smoothstep(0.0f, 0.40f + 0.30f * hill, localgeology + 0.20f * geologydetail),
                    ridgefringe = smoothstep(configuredthreshold - footwidth * 3.5f, configuredthreshold - footwidth * 0.75f, chainstrength) *
                                  (1.0f - smoothstep(0.25f, 0.85f, mountainregion)),
                    satellitehills =
                        (0.08f + 0.16f * hill) * ridgefringe * hillpatch * hillpatch * (0.55f + 0.45f * smoothstep(-0.55f, 0.55f, geologydetail)),
                    mountainrelief = foothills + mainridges + surroundingpeaks + localsummits + satellitehills,
                    amplitudeconversion = settings.maxlanduplift > 0.0f ? settings.mountainmaxamplitude / settings.maxlanduplift : 0.0f,
                    trenchpotential = sample.activity * deepoceanmask;

        // Overlapping ridges can exceed the nominal uplift. Preserve their contours here;
        // the final surface approaches the world ceiling smoothly after all relief is added.
        sample.landuplift = max(landmask * (backgroundrelief + amplitudeconversion * mountainrelief), 0.0f);
        sample.grassplateau = grassplateauweight(localgeology);
        sample.grassplateaudetail = geologydetail;
        sample.terrainroughness = clamp(landmask * (0.22f * hillregion + 0.52f * mountainregion + 0.26f * summitregion) *
                                            (0.72f + 0.28f * max(primaryridge, secondaryridge)),
                                        0.0f, 1.0f);

        const float structuralzone = landmask * hillregion * (0.25f + 0.75f * mountainregion);
        if(structuralzone > 0.001f)
        {
            const float secondarya = smoothstep(0.76f, 0.96f, 1.0f - fabs(generator.secondarysummita.GetNoise(noisex, noisey))),
                        secondaryb = smoothstep(0.76f, 0.96f, 1.0f - fabs(generator.secondarysummitb.GetNoise(noisex, noisey))),
                        secondarysummit = landmask * mountainregion * (0.40f + 0.60f * primaryridge) * secondarya * secondaryb,
                        hollowvalue = -generator.hollowshape.GetNoise(noisex, noisey), hollowcore = smoothstep(0.25f, 0.65f, hollowvalue),
                        hollowedge = smoothstep(0.20f, 0.27f, hollowvalue) * (1.0f - smoothstep(0.30f, 0.36f, hollowvalue)),
                        primaryflank = 4.0f * primaryridge * (1.0f - primaryridge), secondaryflank = 4.0f * secondaryridge * (1.0f - secondaryridge),
                        steepregion = smoothstep(0.60f, 0.84f, sample.terrainroughness),
                        steepflank = smoothstep(0.60f, 0.88f, max(primaryflank, secondaryflank)),
                        ledgeselector = clamp(generator.terrainmicromask.GetNoise(noisex + 7300.0f, noisey - 7300.0f) * 0.5f + 0.5f, 0.0f, 1.0f),
                        ledgepresence = smoothstep(0.56f, 0.76f, ledgeselector),
                        ledgebump =
                            0.65f + 0.55f * clamp(generator.terrainmicro.GetNoise(noisex - 4100.0f, noisey + 4100.0f) * 0.5f + 0.5f, 0.0f, 1.0f);

            // Stretched fields share the tectonically warped frame. Fold ridges
            // run along local Y; the sparse zero contours sampled along local X
            // form transverse cluses that notch through those anticlines.
            const float foldx = tectonicx * generator.foldcos - tectonicy * generator.foldsin,
                        foldy = tectonicx * generator.foldsin + tectonicy * generator.foldcos,
                        foldridge = powf(clamp(1.0f - fabs(generator.foldnoise.GetNoise(foldx, foldy * 0.22f)), 0.0f, 1.0f), 3.0f),
                        foldshoulder = smoothstep(0.38f, 0.72f, foldridge), foldcrest = smoothstep(0.75f, 0.92f, foldridge),
                        crossridge = powf(clamp(1.0f - fabs(generator.clusenoise.GetNoise(foldx * 0.18f, foldy)), 0.0f, 1.0f), 5.0f),
                        crosscut = smoothstep(0.72f, 0.93f, crossridge), cluse = structuralzone * crosscut * (0.35f + 0.65f * foldshoulder),
                        ledge = structuralzone * max(hollowedge, 0.70f * foldcrest) * steepregion * steepflank * ledgepresence *
                                (1.0f - 0.85f * crosscut);

            sample.terrainstructure = settings.secondarysummitheight * secondarysummit + settings.rockyledgeheight * ledge * ledgebump -
                                      settings.rockyledgeheight * 0.35f * structuralzone * hollowcore - settings.clusedepth * cluse;
            sample.rockyledge = clamp(ledge, 0.0f, 1.0f);
        }
        const float hillzone = landmask * ridgefringe * hillpatch;
        if(hillzone > 0.001f && settings.mountainmaxamplitude > 0.0f && settings.maxlanduplift > 0.0f)
        {
            // Broad top/side rolls use existing fields; only the small outcrops need a finer sample.
            const float micro = generator.terrainmicro.GetNoise(noisex, noisey),
                        peakfoot = smoothstep(0.05f, 0.55f, micro) * smoothstep(0.0f, 0.40f, geologydetail),
                        peakcore = smoothstep(0.35f, 0.65f, micro), smallpeak = peakfoot * (2.0f + 5.0f * peakcore),
                        rolls = 8.0f * geologydetail + 3.0f * (secondaryridge - primaryridge);
            sample.terrainstructure += hillzone * min(settings.mountainmaxamplitude / 160.0f, 1.0f) * (rolls + smallpeak);
            sample.hillrock = hillzone * hillrockweight(geologydetail, micro);
        }
        sample.oceantrench = clamp(trenchpotential * powf(sample.activity, 0.35f), 0.0f, 1.0f);

        const float protecteddepth = max(float(settings.cavemindepth), 12.0f), fulldepth = max(float(settings.cavefulldepth), 20.0f),
                    depthmask = smoothstep(protecteddepth, max(fulldepth, protecteddepth + 1.0f), cavedepth),
                    foundationprotection = 1.0f - min(sample.landuplift, 1.0f) * 0.70f;

        sample.caveexpansion = clamp(sample.activity * depthmask * foundationprotection * settings.tectoniccavestrength, 0.0f, 1.0f);

        return sample;
    }

    worldtectonicsample worldgenerator::tectonics(int x, int y, float cavedepth) const
    {
        const float continental = samplecontinental(*this, x + 10000.5f, y - 10000.5f);
        return sampletectonics(*this, x, y, continental, cavedepth);
    }

    float worldgenerator::beachtransitionwidth(int x, int y) const
    {
        const float noisex = x + 10000.5f, noisey = y - 10000.5f, cliffstrength = samplecliffstrength(*this, noisex, noisey);
        float beachspan, plainrun, plainlevel;
        samplecoastprofile(*this, noisex, noisey, beachspan, plainrun, plainlevel);
        return 2.0f * beachspan * powf(1.0f - cliffstrength, 4.0f);
    }

    float worldgenerator::maxbeachtransitionwidth() const
    {
        return 2.40f * max(float(settings.coastwidth), 1.0f);
    }

    float worldgenerator::coasttransitionwidth(int x, int y) const
    {
        float beachspan, plainrun, plainlevel;
        samplecoastprofile(*this, x + 10000.5f, y - 10000.5f, beachspan, plainrun, plainlevel);

        return 2.0f * beachspan + plainrun + 14.0f;
    }

    float worldgenerator::maxcoasttransitionwidth() const
    {
        // Two maximum-width sand terraces, the longest low grass run, and the
        // final inland blend. Keep the coast-map halo large enough for all of it.
        return maxbeachtransitionwidth() + 44.0f;
    }

    bool worldgenerator::beach(int x, int y) const
    {
        if(settings.coastwidth <= 0) return false;
        const float width = beachtransitionwidth(x, y);
        const int maximumcost = int(floorf(width * 3.0f + 0.5f)), searchradius = int(ceilf(maxbeachtransitionwidth())) + 1;
        for(int dy = -searchradius; dy <= searchradius; ++dy)
            for(int dx = -searchradius; dx <= searchradius; ++dx)
            {
                const int diagonal = min(abs(dx), abs(dy)), straight = max(abs(dx), abs(dy)) - diagonal, cost = diagonal * 4 + straight * 3;
                if(cost > maximumcost) continue;
                const int samplex = x + dx, sampley = y + dy;
                const bool water = height(samplex, sampley) < settings.sealevel;
                if((height(samplex - 1, sampley) < settings.sealevel) != water || (height(samplex + 1, sampley) < settings.sealevel) != water ||
                   (height(samplex, sampley - 1) < settings.sealevel) != water || (height(samplex, sampley + 1) < settings.sealevel) != water)
                    return true;
            }
        return false;
    }

    bool worldgenerator::coast(int x, int y) const
    {
        if(settings.coastwidth <= 0) return false;
        const float noisex = x + 10000.5f, noisey = y - 10000.5f,
                    configuredwidth = max(settings.coastwidth + biomeblend.GetNoise(noisex, noisey) * settings.coastvariation, 0.0f),
                    width = max(configuredwidth, coasttransitionwidth(x, y));
        const int maximumcost = int(floorf(width * 3.0f + 0.5f)),
                  searchradius = max(settings.coastwidth + settings.coastvariation, int(ceilf(maxcoasttransitionwidth()))) + 1;
        for(int dy = -searchradius; dy <= searchradius; ++dy)
            for(int dx = -searchradius; dx <= searchradius; ++dx)
            {
                const int diagonal = min(abs(dx), abs(dy)), straight = max(abs(dx), abs(dy)) - diagonal, cost = diagonal * 4 + straight * 3;
                if(cost > maximumcost) continue;
                const int samplex = x + dx, sampley = y + dy;
                const bool water = height(samplex, sampley) < settings.sealevel;
                if((height(samplex - 1, sampley) < settings.sealevel) != water || (height(samplex + 1, sampley) < settings.sealevel) != water ||
                   (height(samplex, sampley - 1) < settings.sealevel) != water || (height(samplex, sampley + 1) < settings.sealevel) != water)
                    return true;
            }
        return false;
    }

    float worldgenerator::fracturecorridor(int x, int y) const
    {
        return fabs(fracturecorridors.GetNoise(x + 24500.5f, y - 24500.5f));
    }

    int worldgenerator::baseheight(int x, int y, worldtectonicsample *tectonics) const
    {
        const float noisex = x + 10000.5f, noisey = y - 10000.5f;
        const float continental = samplecontinental(*this, noisex, noisey);
        const float threshold = landthreshold(settings);
        const worldtectonicsample tectonicsample = sampletectonics(*this, x, y, continental, 0);
        if(tectonics) *tectonics = tectonicsample;
        float elevation, plaindetailmask = 1.0f, plainrollmask = 1.0f, cliffdetailmask = 0.0f;
        if(continental >= threshold)
        {
            float minimumelevation = 2.0f;
            const float distance = clamp((continental - threshold) / max(1.0f - threshold, 0.001f), 0.0f, 1.0f);
            const float coastrise = smoothstep(0.0f, 0.28f, distance);
            const float inland = smoothstep(0.0f, 0.72f, distance);
            const float hill = clamp(hills.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f);
            elevation = settings.maxcontinentheight * coastrise * (0.55f + 0.30f * inland + 0.15f * hill);
            const float continentalelevation = max(elevation, minimumelevation);

            // Build a deliberate beach cross-section near the continental edge.
            // A local gradient converts continental density into approximate metres
            // inland, keeping the profile deterministic and continuous across chunks.
            const float coastprofilelimit = max(16.0f, min(settings.cliffmaxheight, settings.maxcontinentheight));
            if(elevation < coastprofilelimit)
            {
                const float shoredistance = samplecoastdistance(*this, noisex, noisey, continental);

                float beachspan, plainrun, plainlevel;
                samplecoastprofile(*this, noisex, noisey, beachspan, plainrun, plainlevel);

                const float cliffstrength = samplecliffstrength(*this, noisex, noisey),
                            // Cliff sections progressively consume the beach. At full
                            // strength the first land column can already be exposed rock.
                    effectivebeachspan = beachspan * powf(1.0f - cliffstrength, 4.0f), beachend = 2.0f * effectivebeachspan,
                            sandstepratio = 0.5f + clamp(coastshape.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f) / 6.0f,
                            sandstepstart = beachend * sandstepratio, grassriseend = beachend + min(8.0f, plainrun * 0.5f),
                            plainend = beachend + plainrun, blendend = plainend + 14.0f;

                float normalelevation;
                if(effectivebeachspan > 0.01f && shoredistance < beachend) minimumelevation = 0.0f;
                if(effectivebeachspan > 0.01f && shoredistance < sandstepstart)
                    normalelevation = 0.0f;
                else if(effectivebeachspan > 0.01f && shoredistance < beachend)
                    normalelevation = 1.0f;
                // The first grass column is always level 2 on ordinary coasts.
                // Its slow rise prevents a rounded level-3 plain from skipping
                // an entire vertical cube immediately after the sand.
                else if(shoredistance < grassriseend)
                    normalelevation = 2.0f + (plainlevel - 2.0f) * smoothstep(beachend, grassriseend, shoredistance);
                else if(shoredistance < plainend)
                    normalelevation = plainlevel;
                else
                    normalelevation = plainlevel + (continentalelevation - plainlevel) * smoothstep(plainend, blendend, shoredistance);

                // High original relief is allowed to return sooner, while ordinary
                // shores retain the deliberately broad 2–3 metre grass plain.
                const float reliefpermission = smoothstep(9.0f, 16.0f, continentalelevation);
                normalelevation += (max(continentalelevation, normalelevation) - normalelevation) * reliefpermission;

                const float cliffshape = clamp(coastshape.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                            // Keep the current 7–16 metre distribution at the
                            // default while making the configured value a hard cap.
                    cliffheight = settings.cliffmaxheight * (0.4375f + 0.5625f * cliffshape), cliffrise = smoothstep(-0.75f, 2.0f, shoredistance),
                            // Carry the cliff top inland before easing into the continental surface.
                            // Taller cliffs need broader shoulders and longer slopes, rather than a narrow coastal ridge.
                    cliffplateauend = max(plainend, max(64.0f, cliffheight * 4.0f)),
                            cliffblendend = cliffplateauend + max(128.0f, cliffheight * 10.0f),
                            inlandtarget = max(continentalelevation, normalelevation),
                            cliffblend = smoothstep(cliffplateauend, cliffblendend, shoredistance),
                            cliffplateau = cliffheight + (inlandtarget - cliffheight) * cliffblend,
                            cliffelevation = max(inlandtarget, cliffplateau * cliffrise);

                elevation = normalelevation + (cliffelevation - normalelevation) * cliffstrength;
                plaindetailmask = smoothstep(plainend, blendend, shoredistance);
                plainrollmask = smoothstep(grassriseend, blendend, shoredistance);
                cliffdetailmask = cliffstrength * (1.0f - smoothstep(cliffplateauend, cliffblendend, shoredistance));
            }
            elevation = clamp(elevation, 0.0f, settings.maxcontinentheight) + settings.maxlanduplift * tectonicsample.landuplift +
                        tectonicsample.terrainstructure;
            elevation = max(elevation, 0.0f);

            const float roughness = max(tectonicsample.terrainroughness, cliffdetailmask),
                        detailstrength = settings.plainsmicrovariation * plaindetailmask + settings.reliefmicrovariation * roughness;
            const float rollstrength = 6.0f * settings.plainsmicrovariation * plainrollmask * (1.0f - smoothstep(0.1f, 0.65f, roughness));
            if(rollstrength > 0.0f)
            {
                // Lift broad, smooth rolls above the land floor instead of clipping signed noise into flat lowlands.
                // Fade them beyond the beach terraces and out again where mountain or cliff relief takes over.
                const float roll = clamp(plainsroll.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f);
                elevation = max(elevation, minimumelevation) + roll * rollstrength;
            }
            if(detailstrength > 0.0f) elevation = max(elevation + sampleterrainmicrovariation(*this, noisex, noisey) * detailstrength, 0.0f);
            // Evaluate climate at the continental datum, never through hydrology (which calls baseheight).
            // A stable datum avoids a temperature/height feedback loop at the cold boundary.
            const vec coldpos(float(x) * worldclimate::BLOCK_UNITS, float(y) * worldclimate::BLOCK_UNITS,
                              worldclimate::GROUND_UNITS + (settings.sealevel + continentalelevation) * worldclimate::BLOCK_UNITS);
            const float temperature = environmentclimate.gettemperature(coldpos),
                        // humidity = environmentclimate.gethumidity(coldpos),
                cold = (1.0f - smoothstep(-4.0f, 2.0f, temperature)) * smoothstep(2.0f, 18.0f, continentalelevation),
                        polar = 1.0f - smoothstep(-11.0f, -9.0f, temperature), broad = coldbroad.GetNoise(float(x), float(y)),
                        roll = coldroll.GetNoise(float(x), float(y)), micro = coldmicro.GetNoise(float(x), float(y)),
                        region = coldregions.GetNoise(float(x), float(y)),
                        ridge = powf(1.0f - fabsf(coldroll.GetNoise(x * 1.5f + y * 0.3f, y * 0.45f)), 3.0f),
                        tundra = 10.0f * broad + 18.0f * roll + 2.5f * micro, barren = 12.0f * broad + 8.0f * ridge + 1.5f * micro,
                        channel = smoothstep(0.1f, 0.4f, region) * (1.0f - smoothstep(0.02f, 0.10f, fabsf(micro))),
                        boulders = smoothstep(0.20f, 0.40f, region) * smoothstep(0.40f, 0.72f, coldmicro.GetNoise(float(x) * 8.0f, float(y) * 8.0f)) *
                                   (4.0f + polar * 3.0f),
                        target = boulders + continentalelevation * 0.65f + 8.0f + tundra * (1.0f - polar) + barren * polar - 3.0f * channel;
            elevation += cold * (max(target, minimumelevation) - elevation);
            // Only the beach terraces may fall below sea level +2, including after relief and microvariation.
            elevation = max(elevation, minimumelevation);
            // Shape the completed surface into varied, gently rolling meadows while retaining some fine relief.
            elevation = grassplateauheight(settings.sealevel + elevation, tectonicsample.grassplateau, hill, tectonicsample.grassplateaudetail) -
                        settings.sealevel;
        }
        else
        {
            const float distance = clamp((threshold - continental) / max(threshold + 1.0f, 0.001f), 0.0f, 1.0f);
            const float shelf = smoothstep(0.0f, 0.25f, distance);
            const float deepocean = smoothstep(0.15f, 0.85f, distance);
            elevation = -settings.maxoceandepth * (0.25f * shelf + 0.75f * deepocean);
            elevation = clamp(elevation, -settings.maxoceandepth, 0.0f) - settings.maxoceansubsidence * tectonicsample.oceantrench;

            // Continental fields vary slowly enough that a fractional ocean
            // shelf can otherwise round back to a dry sea-level column. Keep
            // every ocean-side sample submerged by at least one whole block so
            // the coast mask reaches the level-0/+1 sand terraces and grass
            // starts cleanly at level +2.
            elevation = min(elevation, -1.0f);
        }
        // Leave lower slopes intact, then approach 255 monotonically instead of slicing off summits.
        // The shoulder has matching first and second derivatives at 200, so it introduces no ledge.
        // Work in absolute height, including sea level, and apply this after every terrain contribution.
        float surfaceheight = settings.sealevel + elevation;
        if(surfaceheight > 200.0f)
        {
            const float excess = surfaceheight - 200.0f, headroom = 55.0f;
            surfaceheight = 200.0f + headroom * excess / sqrtf(headroom * headroom + excess * excess);
        }
        return clamp(int(floor(surfaceheight + 0.5f)), -255, 255);
    }

    bool worldgenerator::cliff(int x, int y, int height, bool *face) const
    {
        if(face) *face = false;
        const float noisex = x + 10000.5f, noisey = y - 10000.5f, continental = samplecontinental(*this, noisex, noisey),
                    threshold = landthreshold(settings), cliffstrength = samplecliffstrength(*this, noisex, noisey);
        if(continental >= threshold && height >= settings.sealevel + 2 && cliffstrength > 0.25f)
        {
            const float shoredistance = samplecoastdistance(*this, noisex, noisey, continental),
                        cliffshape = clamp(coastshape.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                        cliffheight = settings.cliffmaxheight * (0.4375f + 0.5625f * cliffshape);
            float beachspan, plainrun, plainlevel;
            samplecoastprofile(*this, noisex, noisey, beachspan, plainrun, plainlevel);
            const float plainend = 2.0f * beachspan * powf(1.0f - cliffstrength, 4.0f) + plainrun,
                        plateauend = max(plainend, max(64.0f, cliffheight * 4.0f)), blendend = plateauend + max(128.0f, cliffheight * 10.0f);

            // Back the entire raised coast with stone, including recessed columns along a jagged shoreline.
            if(shoredistance <= blendend)
            {
                if(face && shoredistance < 8.0f)
                {
                    // The rising sea face is bare rock. At its crest, only cap columns that are not below a higher ledge.
                    *face = shoredistance < 2.0f;
                    for(int dy = -1; dy <= 1 && !*face; ++dy)
                        for(int dx = -1; dx <= 1 && !*face; ++dx)
                            if((dx || dy) && this->height(x + dx, y + dy) > height + 1) *face = true;
                }
                return true;
            }
        }
        return false;
    }

    bool worldgenerator::rock(int x, int y, int height) const
    {
        const float low = min(settings.stonelow, settings.stonehigh);
        const float high = max(settings.stonelow, settings.stonehigh);
        if(height >= high) return true;

        if(height > settings.sealevel + 8)
        {
            // Reject most columns cheaply; only potential outcrops need the full ridge/fringe mask.
            const float noisex = x + 10000.5f, noisey = y - 10000.5f, micro = terrainmicro.GetNoise(noisex, noisey);
            if(micro > 0.35f || micro < -0.40f)
            {
                const float detail = foothillgeology.GetNoise(noisex * 2.7f + 1731.0f, noisey * 2.7f - 2917.0f);
                if(hillrockweight(detail, micro) > 0.22f && tectonics(x, y).hillrock > 0.22f) return true;
            }
        }
        if(height <= low) return false;

        // Reuse the same single-octave field as the terrace; retain the normal climate-dependent soil/snow rules.
        const float meadow = height > 100 && height < 150 ? grassplateauweight(foothillgeology.GetNoise(x + 10000.5f, y - 10000.5f)) *
                                                                smoothstep(100.0f, 108.0f, height) * (1.0f - smoothstep(142.0f, 150.0f, height))
                                                          : 0.0f,
                    rockweight = smoothstep(low, high, height) * (1.0f - meadow);
        const float selector = clamp(rockiness.GetNoise(x + 10000.5f, y - 10000.5f) * 1.25f + 0.5f, 0.0f, 1.0f);
        return rockweight > selector;
    }

} // namespace game
