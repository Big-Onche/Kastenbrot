from pathlib import Path
p=Path('src/game/world.h'); s=p.read_text().replace('FastNoiseLite vegetationvariation, snowpatches;', 'FastNoiseLite vegetationvariation, snowpatches;\n        FastNoiseLite biomeintrusions, biomedetail, biomeedgewarp;').replace('        int surfacematerial(int x, int y, int height) const;', '''        int surfacematerial(int x, int y, int height) const;
        // Continuous sand influence and a coherent material threshold, shared by surface and vegetation queries.
        float sandcoverage(int x, int y, int height, const BiomeSample &soil) const;
        float sandthreshold(float x, float y) const;'''); p.write_text(s)
p=Path('src/game/world.cpp'); s=p.read_text().replace('        setupnoise(snowpatches,', '''        setupnoise(biomeintrusions, seed ^ 0x19B5A731, 0.006f, 2, 0.35f);
        setupnoise(biomedetail, seed ^ 0x6A34D815, 0.028f, 2, 0.30f);
        setupnoise(biomeedgewarp, seed ^ 0x4D71C923, 0.003f, 2, 0.35f);
        setupnoise(snowpatches,'''); a=s.index('    int worldgenerator::surfacematerial'); s=s[:a]+'''    static float blenddesertcoverage(float coverage, float relativeheight, float slope, float freshwater)
    {
        // Only transitional climates respond: depressions retain moisture, raised shoulders are more exposed.
        // Slope is a weak drying influence, not a rule that every mountain must be desert.
        const float edge = 4.0f * coverage * (1.0f - coverage),
                    terrain = 0.14f * clamp(relativeheight / 8.0f, -1.0f, 1.0f) +
                              0.04f * smoothstep(0.15f, 0.8f, slope) - 0.22f * freshwater;
        return clamp(coverage + edge * terrain, 0.0f, 1.0f);
    }

    float worldgenerator::sandcoverage(int x, int y, int height, const BiomeSample &soil) const
    {
        const float sand = soil.weights[WORLD_BIOME_DESERT];
        float competitor = 0;
        loopi(climateBiomeCount) if(climateBiomes[i].type != WORLD_BIOME_DESERT)
            competitor = max(competitor, soil.weights[climateBiomes[i].type]);
        const float coverage = smoothstep(0.20f, 0.80f, sand / max(sand + competitor, 0.000001f));
        if(coverage <= 0.0f || coverage >= 1.0f) return coverage;

        // Broad relief, not individual voxel steps. Use uncarved heights to avoid recursively routing rivers.
        const float west = baseheight(x - 24, y), east = baseheight(x + 24, y),
                    south = baseheight(x, y - 24), north = baseheight(x, y + 24),
                    relativeheight = height - (west + east + south + north) * 0.25f,
                    slope = sqrtf((east - west) * (east - west) + (north - south) * (north - south)) / 48.0f;
        if(!hydrology) hydrology = new worldhydrology(*this);
        return blenddesertcoverage(coverage, relativeheight, slope, hydrology->moisture(x, y, height));
    }

    float worldgenerator::sandthreshold(float x, float y) const
    {
        // Warped low-frequency lobes produce connected intrusions; smaller detail breaks up their margins.
        // Sampling absolute coordinates preserves the same pattern in chunks, halos and terrain LOD queries.
        const float wx = x + 90.0f * biomeedgewarp.GetNoise(x, y),
                    wy = y + 90.0f * biomeedgewarp.GetNoise(x + 713.0f, y - 419.0f);
        return clamp(0.5f + 0.48f * biomeintrusions.GetNoise(wx, wy) + 0.16f * biomedetail.GetNoise(wx, wy), 0.05f, 0.95f);
    }

'''+s[a:]; s=s.replace('return soil.primary == WORLD_BIOME_DESERT ? WORLD_BIOME_DESERT : WORLD_BIOME_PLAINS;', 'return sandcoverage(x, y, height, soil) > sandthreshold(float(x), float(y)) ? WORLD_BIOME_DESERT : WORLD_BIOME_PLAINS;')
s=s.replace('        const float suitability = treesuitability(sample.temperature, sample.humidity) * ecosystemdensity,', '''        const BiomeSample soil = sampleClimateBiome(environmentclimate.getregionaltemperature(position), sample.humidity);
        const float sand = sandcoverage(x, y, height, soil);
        const float suitability = treesuitability(sample.temperature, sample.humidity) * ecosystemdensity * (1.0f - 0.85f * sand),'''); p.write_text(s)
