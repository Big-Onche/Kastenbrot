// acoustics.cpp: camera environmental acoustics and source occlusion for sound.cpp

#include "engine.h"
#include "AL/efx-presets.h"
#include "acoustics.h"
#include "soundocclusion.h"
#include "worldruntime.h"

namespace acoustics
{
    VARP(soundacoustics, 0, 1, 1);
    VARP(soundacousticsmooth, 0, 1000, 10000);

    FVAR(soundacousticrange, 4.0f, 512.0f, 1024.0f);
    FVAR(soundacousticocclusion, 0.0f, 1.0f, 2.0f);
    FVAR(soundacousticblockgain, 0.05f, 0.15f, 1.0f);
    FVAR(soundacousticmufflegainhf, 0.02f, 0.05f, 1.0f);
    FVARP(soundacousticreverb, 0.0f, 1.0f, 2.0f);

    VARP(soundacousticrays, 16, 128, 256);
    VARP(soundacousticinterval, 1, 250, 10000);

    VARP(soundacousticastarcellsize, 8, 32, 256);
    VARP(soundacousticastarrange, 0, 512, 4096);
    VARP(soundacousticastarbudget, 0, 256, 100000);

    VAR(debugsoundacoustics, 0, 0, 3);
    VARP(debugsoundacousticsradius, 32, 512, 1024);

    static const float SoundUnitsPerMeter = 5.0f;

    static float unitsToMeters(float units)
    {
        return max(units, 0.0f) / SoundUnitsPerMeter;
    }

    static float metersToUnits(float meters)
    {
        return max(meters, 0.0f) * SoundUnitsPerMeter;
    }

    static float rampfactor(float x, float low, float high)
    {
        if(high <= low)
            return x >= high ? 1.0f : 0.0f;
        return clamp((x - low) / (high - low), 0.0f, 1.0f);
    }

    static float smoothramp(float x, float low, float high)
    {
        float t = rampfactor(x, low, high);
        return t * t * (3.0f - 2.0f * t);
    }
    enum
    {
        AP_SMALLROOM = 0,
        AP_HALL,
        AP_CORRIDOR,
        AP_CAVE,
        AP_OPENOUTDOOR,
        AP_COURTYARD,
        AP_STREET,
        AP_CANYON,
        AP_NUM
    };

    enum
    {
        AS_NORTH = 0,
        AS_NORTHEAST,
        AS_EAST,
        AS_SOUTHEAST,
        AS_SOUTH,
        AS_SOUTHWEST,
        AS_WEST,
        AS_NORTHWEST,
        AS_NUM
    };

    struct AcousticPreset
    {
        const char *name;
        EFXEAXREVERBPROPERTIES efx;
    };

    static const AcousticPreset acousticPresets[AP_NUM] = {{"small_room", EFX_REVERB_PRESET_SPACESTATION_SMALLROOM},
                                                           {"hall", EFX_REVERB_PRESET_CASTLE_HALL},
                                                           {"corridor", EFX_REVERB_PRESET_STONECORRIDOR},
                                                           {"cave", EFX_REVERB_PRESET_CAVE},
                                                           {"open_outdoor", EFX_REVERB_PRESET_OUTDOORS_ROLLINGPLAINS},
                                                           {"courtyard", EFX_REVERB_PRESET_CASTLE_COURTYARD},
                                                           {"street", EFX_REVERB_PRESET_CITY_STREETS},
                                                           {"canyon", EFX_REVERB_PRESET_OUTDOORS_DEEPCANYON}};

    struct AcousticChoice
    {
        int first, second;
        float secondWeight;

        AcousticChoice() : first(0), second(0), secondWeight(0) {}
    };

    struct AcousticEvaluation
    {
        float skyOpenness, hitRatio, nearWallRatio, nearDistance, medianDistance, farDistance, distanceVariance, horizontalHitRatio,
            horizontalOpenRatio, horizontalFarHitRatio, medianHorizontalDistance, downOpenness, ceilingDistance, corridorScore, sectorCorridorScore,
            cornerScore, courtyardScore, openPlainScore, boxRoomScore, pcaAnisotropy, pcaVerticality, presetScores[AP_NUM], presetBlend, reverbGain,
            reverbDecay, reflection, outdoorRatio;
        int primaryPreset, secondaryPreset;
        EFXEAXREVERBPROPERTIES reverbShape;

        AcousticEvaluation()
            : skyOpenness(0), hitRatio(0), nearWallRatio(0), nearDistance(0), medianDistance(0), farDistance(0), distanceVariance(0),
              horizontalHitRatio(0), horizontalOpenRatio(0), horizontalFarHitRatio(0), medianHorizontalDistance(0), downOpenness(0),
              ceilingDistance(0), corridorScore(0), sectorCorridorScore(0), cornerScore(0), courtyardScore(0), openPlainScore(0), boxRoomScore(0),
              pcaAnisotropy(0), pcaVerticality(0), presetBlend(0), reverbGain(0), reverbDecay(0.3f), reflection(0), outdoorRatio(1),
              primaryPreset(AP_OPENOUTDOOR), secondaryPreset(AP_OPENOUTDOOR)
        {
            loopi(AP_NUM)
                presetScores[i] = 0.0f;
            EFXEAXREVERBPROPERTIES generic = EFX_REVERB_PRESET_GENERIC;
            reverbShape = generic;
        }
    };

    struct AcousticAStarNode
    {
        int prev;
        float g, f;
        schar passable;
        bool open, closed;

        AcousticAStarNode() : prev(-1), g(1e16f), f(1e16f), passable(-1), open(false), closed(false) {}
    };

    struct AcousticAStarQueueNode
    {
        int cell;
        float f;

        AcousticAStarQueueNode() {}
        AcousticAStarQueueNode(int cell, float f) : cell(cell), f(f) {}
    };

    static inline float heapscore(const AcousticAStarQueueNode &node)
    {
        return node.f;
    }

    struct AcousticAStarResult
    {
        vector<vec> points;
        vec virtualPosition;
        float occlusion, complexity;
        bool found;

        AcousticAStarResult() : virtualPosition(0, 0, 0), occlusion(0), complexity(0), found(false) {}
    };

    struct AcousticRaySample
    {
        vec origin, direction;
        float distance;
    };

    struct AcousticReverb
    {
        float reverbGain, reverbDecay, reflection;
        EFXEAXREVERBPROPERTIES reverbShape;

        AcousticReverb() : reverbGain(0), reverbDecay(0.3f), reflection(0)
        {
            EFXEAXREVERBPROPERTIES generic = EFX_REVERB_PRESET_GENERIC;
            reverbShape = generic;
        }
    };

    static AcousticReverb acousticProbe, acousticTransitionStart, acousticTarget;
    static AcousticEvaluation acousticEvaluation;
    static vector<AcousticRaySample> acousticSamples;
    static int acousticLastMillis = -1, acousticLastDebugMillis = 0, acousticTransitionMillis = 0, acousticSampleCount = 0,
               acousticSampleInterval = 0;
    static float acousticSampleRange = 0;
    static int acousticRayCredit = 0;
    static bool acousticReady = false;
    static vector<vec> acousticDebugPath;
    static vec acousticDebugVirtualSource(0, 0, 0);
    static int acousticAStarFrame = -1, acousticAStarNodesThisFrame = 0, acousticDebugPathMillis = 0, acousticAStarBudgetSamples = 0,
               acousticAStarBudgetNodes = 0;

    static float acousticPercentile(vector<float> &values, float p, float fallback)
    {
        if(values.empty())
            return fallback;
        values.sort();
        float pos = clamp(p, 0.0f, 1.0f) * (values.length() - 1), frac = pos - floorf(pos);
        int lo = clamp(int(pos), 0, values.length() - 1), hi = min(lo + 1, values.length() - 1);
        return values[lo] + (values[hi] - values[lo]) * frac;
    }

    static EFXEAXREVERBPROPERTIES blendEfx(const EFXEAXREVERBPROPERTIES &a, const EFXEAXREVERBPROPERTIES &b, float t)
    {
        t = clamp(t, 0.0f, 1.0f);
        EFXEAXREVERBPROPERTIES out;
#define BLEND_EFX_FIELD(name) out.name = a.name + (b.name - a.name) * t
        BLEND_EFX_FIELD(flDensity);
        BLEND_EFX_FIELD(flDiffusion);
        BLEND_EFX_FIELD(flGain);
        BLEND_EFX_FIELD(flGainHF);
        BLEND_EFX_FIELD(flGainLF);
        BLEND_EFX_FIELD(flDecayTime);
        BLEND_EFX_FIELD(flDecayHFRatio);
        BLEND_EFX_FIELD(flDecayLFRatio);
        BLEND_EFX_FIELD(flReflectionsGain);
        BLEND_EFX_FIELD(flReflectionsDelay);
        loopi(3)
            BLEND_EFX_FIELD(flReflectionsPan[i]);
        BLEND_EFX_FIELD(flLateReverbGain);
        BLEND_EFX_FIELD(flLateReverbDelay);
        loopi(3)
            BLEND_EFX_FIELD(flLateReverbPan[i]);
        BLEND_EFX_FIELD(flEchoTime);
        BLEND_EFX_FIELD(flEchoDepth);
        BLEND_EFX_FIELD(flModulationTime);
        BLEND_EFX_FIELD(flModulationDepth);
        BLEND_EFX_FIELD(flAirAbsorptionGainHF);
        BLEND_EFX_FIELD(flHFReference);
        BLEND_EFX_FIELD(flLFReference);
        BLEND_EFX_FIELD(flRoomRolloffFactor);
#undef BLEND_EFX_FIELD
        out.iDecayHFLimit = t < 0.5f ? a.iDecayHFLimit : b.iDecayHFLimit;
        return out;
    }

    static AcousticChoice chooseTopAcousticPresets(const float *scores, int fallback)
    {
        AcousticChoice choice;
        choice.first = fallback;
        choice.second = fallback;
        float best = -1.0f, next = -1.0f;
        loopi(AP_NUM)
        {
            float score = scores[i];
            if(score > best)
            {
                next = best;
                choice.second = choice.first;
                best = score;
                choice.first = i;
            }
            else if(score > next)
            {
                next = score;
                choice.second = i;
            }
        }
        if(best <= 1e-4f)
        {
            choice.first = choice.second = fallback;
            choice.secondWeight = 0.0f;
            return choice;
        }
        if(next <= 1e-4f || choice.second == choice.first)
        {
            choice.second = choice.first;
            choice.secondWeight = 0.0f;
            return choice;
        }
        float total = best + next;
        choice.secondWeight = next / total;
        return choice;
    }

    static void normalizeAcousticScores(float *scores)
    {
        float total = 0.0f;
        loopi(AP_NUM)
            total += max(scores[i], 0.0f);
        if(total > 1e-4f)
            loopi(AP_NUM)
                scores[i] = max(scores[i], 0.0f) / total;
    }

    static void updatePresetChoice(AcousticEvaluation &evaluation)
    {
        normalizeAcousticScores(evaluation.presetScores);
        AcousticChoice choice = chooseTopAcousticPresets(evaluation.presetScores, evaluation.outdoorRatio >= 0.5f ? AP_OPENOUTDOOR : AP_HALL);
        evaluation.primaryPreset = choice.first;
        evaluation.secondaryPreset = choice.second;
        evaluation.presetBlend = choice.secondWeight;
        evaluation.reverbShape =
            blendEfx(acousticPresets[evaluation.primaryPreset].efx, acousticPresets[evaluation.secondaryPreset].efx, evaluation.presetBlend);
        evaluation.reverbDecay = evaluation.reverbShape.flDecayTime;
    }

    static void scoreAcousticEnvironment(AcousticEvaluation &evaluation)
    {
        float hits = evaluation.hitRatio, skyOpen = evaluation.skyOpenness, ceilingDist = evaluation.ceilingDistance,
              horizontalHitRatio = evaluation.horizontalHitRatio, horizontalNearRatio = evaluation.nearWallRatio,
              horizontalFarHitRatio = evaluation.horizontalFarHitRatio, horizontalOpenRatio = evaluation.horizontalOpenRatio,
              medianHorizontalDistance = evaluation.medianHorizontalDistance, horizontalVariance = evaluation.distanceVariance,
              nearPercentile = evaluation.nearDistance, farPercentile = evaluation.farDistance, corridorScore = evaluation.corridorScore,
              downOpenRatio = evaluation.downOpenness;
        float varianceScore = clamp(sqrtf(horizontalVariance) / max(metersToUnits(medianHorizontalDistance), 1.0f), 0.0f, 1.0f),
              percentileSpread = clamp((farPercentile - nearPercentile) / max(farPercentile, 1.0f), 0.0f, 1.0f),
              irregularityScore = clamp(
                  varianceScore * 0.50f + percentileSpread * 0.30f + evaluation.cornerScore * 0.10f + evaluation.pcaAnisotropy * 0.10f, 0.0f, 1.0f),
              skyOutdoor = smoothramp(skyOpen, 0.15f, 0.65f), openSkyOutdoor = smoothramp(skyOpen, 0.15f, 0.85f),
              skyOnlyOutdoor = openSkyOutdoor * clamp(0.35f + skyOpen * 0.50f + horizontalOpenRatio * 0.10f + downOpenRatio * 0.05f, 0.25f, 0.95f),
              outdoorRaw = clamp(max(skyOutdoor * 0.35f, skyOnlyOutdoor), 0.0f, 1.0f), indoorRaw = 1.0f - outdoorRaw, ceilingOpen = skyOpen,
              fill = clamp(horizontalNearRatio + horizontalHitRatio - horizontalFarHitRatio, 0.0f, 1.0f),
              roomSizeScore = 1.0f - smoothramp(medianHorizontalDistance, 6.0f, 24.0f);

        evaluation.presetScores[AP_SMALLROOM] = indoorRaw * (0.35f + horizontalHitRatio * 0.65f) * roomSizeScore *
                                                (1.0f - irregularityScore * 0.45f) * (1.0f - horizontalFarHitRatio * 0.35f) *
                                                (0.75f + evaluation.boxRoomScore * 0.35f);
        evaluation.presetScores[AP_HALL] = indoorRaw * (0.30f + horizontalHitRatio * 0.70f) * smoothramp(medianHorizontalDistance, 8.0f, 28.0f) *
                                           (0.40f + horizontalFarHitRatio * 0.60f) * (1.0f - corridorScore * 0.75f) *
                                           (1.0f - irregularityScore * 0.35f) * (0.90f + evaluation.boxRoomScore * 0.20f);

        evaluation.presetScores[AP_CORRIDOR] = indoorRaw * (0.20f + horizontalHitRatio * 0.80f) * corridorScore *
                                               (0.55f + fill * 0.45f)                        // slightly stronger fill reinforcement
                                               * (1.0f - ceilingOpen * 0.40f)                // penalise open/tall ceilings (halls have those)
                                               * (0.90f + evaluation.pcaAnisotropy * 0.65f); // stronger directional reinforcement

        float lowCeiling = 1.0f - smoothramp(ceilingDist, metersToUnits(2.0f), metersToUnits(6.0f));
        evaluation.presetScores[AP_CAVE] = indoorRaw * (0.20f + hits * 0.45f + horizontalFarHitRatio * 0.20f) // merged far hits in
                                           * smoothramp(irregularityScore, 0.25f, 0.70f)                      // lower entry threshold
                                           * (0.40f + lowCeiling * 0.60f)                                     // explicit low-ceiling signal
                                           * (1.0f - corridorScore * 0.55f) *
                                           (0.80f + evaluation.pcaVerticality * 0.40f); // stronger verticality boost

        evaluation.presetScores[AP_OPENOUTDOOR] = outdoorRaw * skyOpen * (0.45f + horizontalOpenRatio * 0.55f) * (1.0f - horizontalNearRatio) *
                                                  (1.0f - corridorScore * 0.50f) * (0.75f + evaluation.openPlainScore * 0.45f);
        evaluation.presetScores[AP_COURTYARD] = outdoorRaw * skyOpen * (0.25f + horizontalNearRatio * 0.75f) * (0.25f + horizontalHitRatio * 0.75f) *
                                                (1.0f - corridorScore * 0.40f) * (0.70f + evaluation.courtyardScore * 0.60f);
        evaluation.presetScores[AP_STREET] = outdoorRaw * skyOpen * (0.25f + horizontalNearRatio * 0.75f) *
                                             max(corridorScore, evaluation.sectorCorridorScore) * (0.85f + evaluation.pcaAnisotropy * 0.35f);
        evaluation.presetScores[AP_CANYON] = outdoorRaw * skyOpen * (0.25f + horizontalFarHitRatio * 0.75f) * (0.35f + irregularityScore * 0.65f) *
                                             (0.55f + max(corridorScore, evaluation.pcaAnisotropy) * 0.45f) * (0.70f + downOpenRatio * 0.30f);

        evaluation.presetScores[AP_SMALLROOM] *= 0.72f;
        evaluation.presetScores[AP_HALL] *= 1.14f;
        evaluation.presetScores[AP_CORRIDOR] *= 1.28f;
        evaluation.presetScores[AP_CAVE] *= 1.22f;
        normalizeAcousticScores(evaluation.presetScores);

        if(indoorRaw > 0.2f && evaluation.presetScores[AP_SMALLROOM] + evaluation.presetScores[AP_HALL] + evaluation.presetScores[AP_CORRIDOR] +
                                       evaluation.presetScores[AP_CAVE] <=
                                   1e-4f)
            evaluation.presetScores[medianHorizontalDistance < 10.0f ? AP_SMALLROOM : AP_HALL] = indoorRaw;
        if(outdoorRaw > 0.2f && evaluation.presetScores[AP_OPENOUTDOOR] + evaluation.presetScores[AP_COURTYARD] + evaluation.presetScores[AP_STREET] +
                                        evaluation.presetScores[AP_CANYON] <=
                                    1e-4f)
            evaluation.presetScores[AP_OPENOUTDOOR] = outdoorRaw;
        evaluation.outdoorRatio = outdoorRaw;
        updatePresetChoice(evaluation);

        float indoorStrength = (1.0f - outdoorRaw) * clamp(0.25f + hits * 0.35f + horizontalFarHitRatio * 0.25f + fill * 0.15f, 0.0f, 1.0f),
              outdoorStrength =
                  outdoorRaw * clamp(0.07f + horizontalNearRatio * 0.35f + horizontalFarHitRatio * 0.30f + corridorScore * 0.25f, 0.04f, 0.75f);

        evaluation.reverbGain = clamp(indoorStrength + outdoorStrength, 0.0f, 1.0f);
        evaluation.reflection = clamp(horizontalNearRatio * 0.35f + horizontalFarHitRatio * 0.25f + corridorScore * 0.30f + hits * 0.15f, 0.0f, 1.0f);
    }

    static int acousticHorizontalSector(const vec &dir)
    {
        float angle = PI / 2.0f - atan2f(dir.y, dir.x);
        if(angle < 0.0f)
            angle += 2.0f * PI;
        else if(angle >= 2.0f * PI)
            angle -= 2.0f * PI;
        return clamp(int(floorf((angle + PI / 8.0f) * (AS_NUM / (2.0f * PI)))) & 7, 0, AS_NUM - 1);
    }

    static void evaluateAcousticSamples(AcousticEvaluation &evaluation, const vector<AcousticRaySample> &samples, float range)
    {
        const int rays = samples.length();
        const float nearDist = metersToUnits(6.0f), farDist = metersToUnits(24.0f), maxHitDist = range * 0.98f;
        float open = 0, hits = 0, skyOpen = 1.0f, ceilingDist = 0, skyCount = 0, skyOpenCount = 0, ceilingHits = 0, skyDiagCount = 0, skyDiagOpen = 0,
              horizontalCount = 0, horizontalHits = 0, horizontalNear = 0, horizontalFar = 0, horizontalFarHits = 0, horizontalDist = 0,
              horizontalDist2 = 0, downCount = 0, downOpen = 0, pcaCount = 0, pcaSumX = 0, pcaSumY = 0, pcaSumZ = 0, pcaXX = 0, pcaXY = 0, pcaYY = 0,
              pcaZZ = 0;
        vector<float> hitDistances, horizontalDistances;

        struct AcousticSector
        {
            int count, hits;
            float dist, nearHits;

            AcousticSector() : count(0), hits(0), dist(0), nearHits(0) {}
        } sectors[AS_NUM];

        loopi(rays)
        {
            const vec &dir = samples[i].direction;
            float dist = samples[i].distance;
            bool hit = dist < maxHitDist;
            vec sample = vec(dir).mul(dist);
            pcaCount += 1.0f;
            pcaSumX += sample.x;
            pcaSumY += sample.y;
            pcaSumZ += sample.z;
            pcaXX += sample.x * sample.x;
            pcaXY += sample.x * sample.y;
            pcaYY += sample.y * sample.y;
            pcaZZ += sample.z * sample.z;
            open += dist / range;
            if(hit)
            {
                hits += 1.0f;
                hitDistances.add(unitsToMeters(dist));
            }
            if(dir.z > 0.25f)
            {
                skyDiagCount += 1.0f;
                skyDiagOpen += hit ? smoothramp(dist / range, 0.78f, 0.98f) : 1.0f;
            }
            if(dir.z > 0.65f)
            {
                skyCount += 1.0f;
                if(!hit)
                    skyOpenCount += 1.0f;
                else
                {
                    ceilingHits += 1.0f;
                    ceilingDist += dist;
                }
            }
            else if(fabs(dir.z) < 0.30f)
            {
                horizontalCount += 1.0f;
                horizontalDist += dist;
                horizontalDist2 += dist * dist;
                horizontalDistances.add(unitsToMeters(dist));
                if(hit)
                {
                    horizontalHits += 1.0f;
                    if(dist <= nearDist)
                        horizontalNear += 1.0f;
                    if(dist >= farDist)
                        horizontalFarHits += 1.0f;
                }
                if(dist >= farDist)
                    horizontalFar += 1.0f;

                int sector = acousticHorizontalSector(dir);
                sectors[sector].count++;
                sectors[sector].dist += dist;
                if(hit)
                {
                    sectors[sector].hits++;
                    if(dist <= nearDist)
                        sectors[sector].nearHits += 1.0f;
                }
            }
            else if(dir.z < -0.35f)
            {
                downCount += 1.0f;
                if(!hit)
                    downOpen += 1.0f;
            }
        }

        open /= rays;
        hits /= rays;
        if(skyCount > 0)
            skyOpen = skyOpenCount / skyCount;
        float verticalSkyOpen = skyOpen, diagonalSkyOpen = skyDiagCount > 0 ? skyDiagOpen / skyDiagCount : verticalSkyOpen;
        skyOpen = clamp(max(verticalSkyOpen * 0.65f + diagonalSkyOpen * 0.35f, diagonalSkyOpen * 0.55f), 0.0f, 1.0f);
        if(ceilingHits > 0)
            ceilingDist /= ceilingHits;
        else
            ceilingDist = range;

        float horizontalHitRatio = horizontalCount > 0 ? horizontalHits / horizontalCount : hits,
              horizontalNearRatio = horizontalCount > 0 ? horizontalNear / horizontalCount : 0.0f,
              horizontalFarRatio = horizontalCount > 0 ? horizontalFar / horizontalCount : open,
              horizontalFarHitRatio = horizontalCount > 0 ? horizontalFarHits / horizontalCount : 0.0f,
              horizontalOpenRatio = max(horizontalFarRatio - horizontalFarHitRatio, 0.0f),
              horizontalAvgDist = horizontalCount > 0 ? horizontalDist / horizontalCount : open * range, horizontalVariance = 0.0f,
              downOpenRatio = downCount > 0 ? downOpen / downCount : 0.0f;
        if(horizontalCount > 1)
        {
            float mean = horizontalAvgDist;
            horizontalVariance = max(horizontalDist2 / horizontalCount - mean * mean, 0.0f);
        }

        float avgHitDistance = hitDistances.empty() ? unitsToMeters(horizontalAvgDist) : 0.0f;
        loopv(hitDistances)
            avgHitDistance += hitDistances[i];
        if(!hitDistances.empty())
            avgHitDistance /= hitDistances.length();
        float nearPercentile = acousticPercentile(hitDistances, 0.25f, avgHitDistance),
              medianHitDistance = acousticPercentile(hitDistances, 0.50f, avgHitDistance),
              farPercentile = acousticPercentile(hitDistances, 0.75f, avgHitDistance),
              medianHorizontalDistance = acousticPercentile(horizontalDistances, 0.50f, unitsToMeters(horizontalAvgDist));

        float sectorOpen[AS_NUM], sectorNear[AS_NUM];
        loopi(AS_NUM)
        {
            sectorOpen[i] = sectors[i].count ? clamp(sectors[i].dist / (sectors[i].count * range), 0.0f, 1.0f) : open;
            sectorNear[i] = sectors[i].count ? clamp(sectors[i].nearHits / sectors[i].count, 0.0f, 1.0f) : horizontalNearRatio;
        }
        float sectorCorridor = 0.0f;
        loopi(4)
        {
            int a = i, b = i + 4, side1 = (i + 2) & 7, side2 = (i + 6) & 7;
            float alongFar = min(sectorOpen[a], sectorOpen[b]), sideClosed = 1.0f - 0.5f * (sectorOpen[side1] + sectorOpen[side2]),
                  sideNear = 0.5f * (sectorNear[side1] + sectorNear[side2]);
            sectorCorridor = max(sectorCorridor, clamp(alongFar * (sideClosed * 0.55f + sideNear * 0.45f), 0.0f, 1.0f));
        }
        float sectorMean = 0.0f, sectorVar = 0.0f;
        loopi(AS_NUM)
        {
            sectorMean += sectorOpen[i];
        }
        sectorMean /= AS_NUM;
        loopi(AS_NUM)
            sectorVar += (sectorOpen[i] - sectorMean) * (sectorOpen[i] - sectorMean);
        sectorVar /= AS_NUM;
        float cornerScore = 0.0f;
        loopi(AS_NUM)
        {
            int next = (i + 1) & 7, away = (i + 4) & 7, awaynext = (i + 5) & 7;
            cornerScore = max(cornerScore, min(sectorNear[i], sectorNear[next]) * max(sectorOpen[away], sectorOpen[awaynext]));
        }
        float pcaAnisotropy = 0.0f, pcaVerticality = 0.0f;
        if(pcaCount > 1.0f)
        {
            float inv = 1.0f / pcaCount, meanX = pcaSumX * inv, meanY = pcaSumY * inv, meanZ = pcaSumZ * inv,
                  cxx = max(pcaXX * inv - meanX * meanX, 0.0f), cxy = pcaXY * inv - meanX * meanY, cyy = max(pcaYY * inv - meanY * meanY, 0.0f),
                  czz = max(pcaZZ * inv - meanZ * meanZ, 0.0f), trace = cxx + cyy,
                  delta = sqrtf(max((cxx - cyy) * (cxx - cyy) + 4.0f * cxy * cxy, 0.0f)), major = max(0.5f * (trace + delta), 0.0f),
                  minor = max(0.5f * (trace - delta), 0.0f), total = max(major + minor + czz, 1.0f);
            pcaAnisotropy = clamp((major - minor) / max(major + minor, 1.0f), 0.0f, 1.0f);
            pcaVerticality = clamp(czz / total, 0.0f, 1.0f);
        }
        float varianceScore = clamp(sqrtf(horizontalVariance) / max(horizontalAvgDist, 1.0f), 0.0f, 1.0f),
              corridorScore =
                  clamp(max(max(min(horizontalNearRatio, horizontalFarRatio) * max(varianceScore, 0.25f), smoothramp(sectorCorridor, 0.06f, 0.36f)),
                            pcaAnisotropy * (0.25f + horizontalNearRatio * 0.55f)),
                        0.0f, 1.0f),
              balancedSectors = 1.0f - clamp(sqrtf(sectorVar) * 2.5f, 0.0f, 1.0f);

        evaluation.skyOpenness = skyOpen;
        evaluation.hitRatio = hits;
        evaluation.nearWallRatio = horizontalNearRatio;
        evaluation.horizontalHitRatio = horizontalHitRatio;
        evaluation.horizontalOpenRatio = horizontalOpenRatio;
        evaluation.horizontalFarHitRatio = horizontalFarHitRatio;
        evaluation.nearDistance = nearPercentile;
        evaluation.medianDistance = medianHitDistance;
        evaluation.farDistance = farPercentile;
        evaluation.medianHorizontalDistance = medianHorizontalDistance;
        evaluation.distanceVariance = horizontalVariance;
        evaluation.downOpenness = downOpenRatio;
        evaluation.ceilingDistance = ceilingDist;
        evaluation.sectorCorridorScore = smoothramp(sectorCorridor, 0.06f, 0.36f);
        evaluation.cornerScore = clamp(cornerScore, 0.0f, 1.0f);
        evaluation.courtyardScore = clamp(skyOpen * horizontalHitRatio * balancedSectors * (0.45f + horizontalNearRatio * 0.55f), 0.0f, 1.0f);
        evaluation.openPlainScore = clamp(skyOpen * horizontalOpenRatio * (1.0f - horizontalNearRatio) * (1.0f - pcaAnisotropy * 0.65f), 0.0f, 1.0f);
        evaluation.boxRoomScore = clamp(
            (1.0f - skyOpen) * horizontalHitRatio * balancedSectors * (1.0f - varianceScore) * (1.0f - horizontalOpenRatio * 0.45f), 0.0f, 1.0f);
        evaluation.pcaAnisotropy = pcaAnisotropy;
        evaluation.pcaVerticality = pcaVerticality;
        evaluation.corridorScore = corridorScore;

        scoreAcousticEnvironment(evaluation);
    }

    static void advanceAcousticAStarFrame(int now)
    {
        if(acousticAStarFrame == now)
            return;
        if(acousticAStarFrame >= 0)
        {
            acousticAStarBudgetNodes += acousticAStarNodesThisFrame;
            acousticAStarBudgetSamples++;
        }
        acousticAStarFrame = now;
        acousticAStarNodesThisFrame = 0;
    }

    static void debugAcousticAStarBudget()
    {
        if(debugsoundacoustics != 2 || totalmillis - acousticLastDebugMillis < 1000)
            return;
        float avg = acousticAStarBudgetSamples > 0 ? float(acousticAStarBudgetNodes) / float(acousticAStarBudgetSamples) : 0.0f;
        acousticLastDebugMillis = totalmillis;
        conoutf(CON_DEBUG, "sound propagation A* budget: average %.1f/%d nodes per frame", avg, max(soundacousticastarbudget, 0));
        acousticAStarBudgetSamples = acousticAStarBudgetNodes = 0;
    }

    void resetAcoustics()
    {
        acousticSamples.setsize(0);
        acousticDebugPath.setsize(0);
        acousticProbe = acousticTransitionStart = acousticTarget = AcousticReverb();
        acousticEvaluation = AcousticEvaluation();
        acousticReady = false;
        acousticLastMillis = -1;
        acousticRayCredit = 0;
        acousticSampleCount = acousticSampleInterval = 0;
        acousticSampleRange = 0;
        acousticAStarFrame = -1;
        acousticAStarNodesThisFrame = acousticAStarBudgetSamples = acousticAStarBudgetNodes = 0;
    }

    void rebaseAcoustics(float shiftx, float shifty)
    {
        vec shift(shiftx, shifty, 0);
        loopv(acousticSamples)
            acousticSamples[i].origin.sub(shift);
        loopv(acousticDebugPath)
            acousticDebugPath[i].sub(shift);
        acousticDebugVirtualSource.sub(shift);
    }

    static void smoothAcousticReverb(int now)
    {
        float t = soundacousticsmooth > 0 ? clamp((now - acousticTransitionMillis) / float(soundacousticsmooth), 0.0f, 1.0f) : 1.0f;
        t = t * t * (3.0f - 2.0f * t);
        acousticProbe.reverbGain = acousticTransitionStart.reverbGain + (acousticTarget.reverbGain - acousticTransitionStart.reverbGain) * t;
        acousticProbe.reverbDecay = acousticTransitionStart.reverbDecay + (acousticTarget.reverbDecay - acousticTransitionStart.reverbDecay) * t;
        acousticProbe.reflection = acousticTransitionStart.reflection + (acousticTarget.reflection - acousticTransitionStart.reflection) * t;
        acousticProbe.reverbShape = blendEfx(acousticTransitionStart.reverbShape, acousticTarget.reverbShape, t);
    }

    static void finishAcousticEvaluation(int now)
    {
        AcousticEvaluation evaluation;
        evaluateAcousticSamples(evaluation, acousticSamples, acousticSampleRange);
        // Identical evaluations must not restart a transition that is still in progress.
        if(!acousticReady || evaluation.primaryPreset != acousticEvaluation.primaryPreset ||
           evaluation.secondaryPreset != acousticEvaluation.secondaryPreset || evaluation.presetBlend != acousticEvaluation.presetBlend ||
           evaluation.reverbGain != acousticTarget.reverbGain || evaluation.reverbDecay != acousticTarget.reverbDecay ||
           evaluation.reflection != acousticTarget.reflection)
        {
            acousticTransitionStart = acousticProbe;
            acousticTransitionMillis = now;
            acousticTarget.reverbGain = evaluation.reverbGain;
            acousticTarget.reverbDecay = evaluation.reverbDecay;
            acousticTarget.reflection = evaluation.reflection;
            acousticTarget.reverbShape = evaluation.reverbShape;
        }
        acousticEvaluation = evaluation;
        acousticReady = true;
        acousticSamples.setsize(0);
    }

    void updateAcoustics()
    {
        int now = totalmillis;
        if(!soundacoustics || !camera1 || !insideworld(camera1->o))
        {
            resetAcoustics();
            sound::updateAcousticReverb(NULL, 0, 0.3f, 0);
            return;
        }
        advanceAcousticAStarFrame(now);
        debugAcousticAStarBudget();
        smoothAcousticReverb(now);
        if(acousticSampleCount != soundacousticrays || acousticSampleInterval != soundacousticinterval || acousticSampleRange != soundacousticrange ||
           acousticLastMillis < 0 || now < acousticLastMillis)
        {
            acousticSamples.setsize(0);
            acousticRayCredit = 0;
            acousticSampleCount = soundacousticrays;
            acousticSampleInterval = soundacousticinterval;
            acousticSampleRange = soundacousticrange;
            acousticLastMillis = now;
        }
        // Fractional credit spreads the casts across frames. Discard missed sweeps after a stall.
        int elapsed = clamp(now - acousticLastMillis, 0, acousticSampleInterval);
        acousticLastMillis = now;
        acousticRayCredit += elapsed * acousticSampleCount;
        int casts = acousticRayCredit / acousticSampleInterval;
        acousticRayCredit %= acousticSampleInterval;
        loopi(casts)
        {
            // Interleave opposite hemispheres instead of sweeping from ceiling to floor over time.
            int sample = acousticSamples.length(), half = (acousticSampleCount + 1) / 2, index = sample % 2 ? half + sample / 2 : sample / 2;
            float z = 1.0f - (2.0f * (index + 0.5f)) / acousticSampleCount, r = sqrtf(max(0.0f, 1.0f - z * z)),
                  angle = PI * (3.0f - sqrtf(5.0f)) * index;
            AcousticRaySample &ray = acousticSamples.add();
            ray.origin = camera1->o;
            ray.direction = vec(cosf(angle) * r, sinf(angle) * r, z);
            ray.distance = clamp(raycube(ray.origin, ray.direction, acousticSampleRange, RAY_POLY), 0.0f, acousticSampleRange);
            if(acousticSamples.length() == acousticSampleCount)
                finishAcousticEvaluation(now);
        }
        smoothAcousticReverb(now);
        sound::updateAcousticReverb(acousticReady ? &acousticProbe.reverbShape : NULL, acousticProbe.reverbGain * soundacousticreverb,
                                    acousticProbe.reverbDecay, acousticProbe.reflection);
        if((debugsoundacoustics == 1 || debugsoundacoustics == 3) && acousticReady && now - acousticLastDebugMillis >= 1000)
        {
            acousticLastDebugMillis = now;
            conoutf(CON_DEBUG, "camera acoustics: %s %d%% / %s %d%%, sky %d%%, median %.1fm, %d rays / %d ms",
                    acousticPresets[acousticEvaluation.primaryPreset].name, int((1.0f - acousticEvaluation.presetBlend) * 100.0f + 0.5f),
                    acousticPresets[acousticEvaluation.secondaryPreset].name, int(acousticEvaluation.presetBlend * 100.0f + 0.5f),
                    int(acousticEvaluation.skyOpenness * 100.0f + 0.5f), acousticEvaluation.medianDistance, acousticSampleCount,
                    acousticSampleInterval);
        }
    }

    static bool acousticAStarBudgetAvailable()
    {
        advanceAcousticAStarFrame(totalmillis);
        return soundacousticastarbudget > 0 && acousticAStarNodesThisFrame < soundacousticastarbudget;
    }

    static ivec acousticAStarCellCoord2D(const vec &o, float cellsize)
    {
        return ivec(int(floorf(o.x / cellsize)), int(floorf(o.y / cellsize)), 0);
    }

    static vec acousticAStarCellCenter2D(const ivec &coord, float z, float cellsize)
    {
        return vec((coord.x + 0.5f) * cellsize, (coord.y + 0.5f) * cellsize, z);
    }

    static float acousticAStarHeuristic2D(const ivec &coord, const ivec &target, float cellsize)
    {
        float dx = float(coord.x - target.x), dy = float(coord.y - target.y);
        return sqrtf(dx * dx + dy * dy);
    }

    static int acousticAStarNodeIndex(hashtable<ivec, int> &lookup, vector<AcousticAStarNode> &nodes, vector<ivec> &coords, const ivec &coord)
    {
        int *idx = lookup.access(coord);
        if(idx)
            return *idx;
        int newidx = nodes.length();
        nodes.add(AcousticAStarNode());
        coords.add(coord);
        lookup[coord] = newidx;
        return newidx;
    }

    static bool acousticAStarPointPassable(const vec &p, float cellsize)
    {
        if(!insideworld(p))
            return false;
        float clearance = max(cellsize * 0.35f, 4.0f);
        static const vec dirs[5] = {vec(1, 0, 0), vec(-1, 0, 0), vec(0, 1, 0), vec(0, -1, 0), vec(0, 0, 1)};
        loopi(sizeof(dirs) / sizeof(dirs[0]))
            if(raycube(p, dirs[i], clearance, RAY_POLY) >= clearance * 0.45f)
                return true;
        return false;
    }

    static bool acousticAStarSegmentPassable(const vec &from, const vec &to, bool streamed = false)
    {
        vec ray = vec(to).sub(from);
        float len = ray.magnitude();
        if(len <= 1e-3f)
            return true;
        ray.div(len);
        if(raycube(from, ray, len, RAY_POLY) < len * 0.96f)
            return false;
        if(!streamed)
            return true;
        return soundworldhit(from, ray, len, float(WORLD_BLOCK_SIZE), [](const ivec &point) {
                   int bottom;
                   return sampleworldsolid(point, bottom);
               }) >= len;
    }

    static bool acousticAStarNodePassable(vector<AcousticAStarNode> &nodes, const vector<ivec> &coords, int idx, float z, float cellsize)
    {
        if(!nodes.inrange(idx) || !coords.inrange(idx))
            return false;
        if(nodes[idx].passable < 0)
        {
            vec center = acousticAStarCellCenter2D(coords[idx], z, cellsize);
            nodes[idx].passable = acousticAStarPointPassable(center, cellsize) ? 1 : 0;
        }
        return nodes[idx].passable > 0;
    }

    static bool acousticAStarCoordInRange(const ivec &coord, const vec &listener, float z, float cellsize, float maxdist)
    {
        vec center = acousticAStarCellCenter2D(coord, z, cellsize);
        float limit = maxdist + cellsize * 1.5f;
        return center.squaredist(listener) <= limit * limit;
    }

    static void buildAcousticAStarResult(const vec &source, const vec &listener, float z, float cellsize, int sourceidx, int targetidx,
                                         const vector<AcousticAStarNode> &nodes, const vector<ivec> &coords, float directOcclusion,
                                         AcousticAStarResult &result)
    {
        vector<int> path;
        for(int cur = targetidx; nodes.inrange(cur); cur = nodes[cur].prev)
        {
            path.add(cur);
            if(cur == sourceidx)
                break;
        }
        path.reverse();
        if(path.empty() || path[0] != sourceidx || path.last() != targetidx)
            return;

        result.points.setsize(0);
        loopv(path)
            if(coords.inrange(path[i]))
                result.points.add(acousticAStarCellCenter2D(coords[path[i]], z, cellsize));
        if(result.points.empty())
            return;

        result.found = true;
        float pathLength = 0.0f;
        loopv(result.points)
            if(i)
                pathLength += result.points[i - 1].dist(result.points[i]);

        float direct = max(source.dist(listener), 1.0f);
        result.complexity = clamp((pathLength / direct - 1.0f) * 0.70f + float(max(result.points.length() - 2, 0)) * 0.025f, 0.0f, 1.0f);
        result.occlusion = clamp(max(directOcclusion, result.complexity * 0.65f) * soundacousticocclusion, 0.0f, 1.0f);

        vec entry = result.points.length() >= 2 ? result.points[result.points.length() - 2] : source, incoming = vec(entry).sub(listener);
        incoming.z = 0;
        if(incoming.iszero())
            incoming = vec(source).sub(listener);
        incoming.z = 0;
        if(!incoming.iszero())
            incoming.safenormalize();
        result.virtualPosition = vec(listener).add(incoming.mul(max(cellsize * 1.5f, 16.0f)));
    }

    static bool findAcousticAStarPath(const vec &loc, float dist, float directOcclusion, AcousticAStarResult &result, bool streamed = false)
    {
        if(!camera1)
            return false;
        if(soundacousticastarrange > 0 && dist > soundacousticastarrange)
            return false;
        if(!acousticAStarBudgetAvailable())
            return false;

        float cellsize = max(float(soundacousticastarcellsize), 8.0f), z = camera1->o.z,
              maxdist = soundacousticastarrange > 0 ? float(soundacousticastarrange) : max(dist + cellsize * 4.0f, cellsize * 8.0f);
        vec source(loc.x, loc.y, z), listener(camera1->o.x, camera1->o.y, z);
        ivec sourcecoord = acousticAStarCellCoord2D(source, cellsize), targetcoord = acousticAStarCellCoord2D(listener, cellsize);
        if(sourcecoord == targetcoord)
            return false;
        if(streamed && (!acousticAStarSegmentPassable(loc, acousticAStarCellCenter2D(sourcecoord, z, cellsize), true) ||
                        !acousticAStarSegmentPassable(acousticAStarCellCenter2D(targetcoord, z, cellsize), camera1->o, true)))
            return false;

        hashtable<ivec, int> lookup(1 << 10);
        vector<AcousticAStarNode> nodes;
        vector<ivec> coords;
        vector<AcousticAStarQueueNode> queue;

        int sourceidx = acousticAStarNodeIndex(lookup, nodes, coords, sourcecoord),
            targetidx = acousticAStarNodeIndex(lookup, nodes, coords, targetcoord);
        nodes[sourceidx].passable = nodes[targetidx].passable = 1;
        nodes[sourceidx].g = 0.0f;
        nodes[sourceidx].f = acousticAStarHeuristic2D(sourcecoord, targetcoord, cellsize);
        nodes[sourceidx].open = true;
        queue.addheap(AcousticAStarQueueNode(sourceidx, nodes[sourceidx].f));

        static const ivec dirs[8] = {ivec(1, 0, 0), ivec(-1, 0, 0), ivec(0, 1, 0),  ivec(0, -1, 0),
                                     ivec(1, 1, 0), ivec(1, -1, 0), ivec(-1, 1, 0), ivec(-1, -1, 0)};

        while(!queue.empty())
        {
            if(!acousticAStarBudgetAvailable())
                return false;
            AcousticAStarQueueNode q = queue.removeheap();
            if(!nodes.inrange(q.cell) || nodes[q.cell].closed || q.f > nodes[q.cell].f + 1e-4f)
                continue;
            acousticAStarNodesThisFrame++;
            if(q.cell == targetidx)
            {
                buildAcousticAStarResult(source, listener, z, cellsize, sourceidx, targetidx, nodes, coords, directOcclusion, result);
                return result.found;
            }

            // Adding neighbors may reallocate nodes; do not retain a reference into that vector.
            const float currentCost = nodes[q.cell].g;
            nodes[q.cell].closed = true;
            vec curCenter = acousticAStarCellCenter2D(coords[q.cell], z, cellsize);
            loopi(sizeof(dirs) / sizeof(dirs[0]))
            {
                ivec nextcoord = ivec(coords[q.cell]).add(dirs[i]);
                if(!acousticAStarCoordInRange(nextcoord, listener, z, cellsize, maxdist))
                    continue;
                int nextidx = acousticAStarNodeIndex(lookup, nodes, coords, nextcoord);
                if(nodes[nextidx].closed || !acousticAStarNodePassable(nodes, coords, nextidx, z, cellsize))
                    continue;
                vec nextCenter = acousticAStarCellCenter2D(nextcoord, z, cellsize);
                if(!acousticAStarSegmentPassable(curCenter, nextCenter, streamed))
                    continue;
                float step = dirs[i].x && dirs[i].y ? 1.41421356f : 1.0f, g = currentCost + step;
                if(nodes[nextidx].open && g >= nodes[nextidx].g - 1e-4f)
                    continue;
                nodes[nextidx].prev = q.cell;
                nodes[nextidx].g = g;
                nodes[nextidx].f = g + acousticAStarHeuristic2D(nextcoord, targetcoord, cellsize);
                nodes[nextidx].open = true;
                queue.addheap(AcousticAStarQueueNode(nextidx, nodes[nextidx].f));
            }
        }
        return false;
    }

    static float acousticDirectPointOcclusion(const vec &from, const vec &to)
    {
        return soundpointocclusion(from, to,
                                   [](const vec &start, const vec &direction, float length) { return raycube(start, direction, length, RAY_POLY); });
    }

    static void applyDirectOcclusion(float occlusion, float &volf, float &gainhf)
    {
        float occ = clamp(occlusion * soundacousticocclusion, 0.0f, 1.0f), closedGainHF = clamp(soundacousticmufflegainhf, 0.02f, 1.0f),
              occVol = powf(occ, 0.70f), occHF = powf(occ, 0.35f);
        volf *= 1.0f - occVol * (1.0f - soundacousticblockgain);
        gainhf *= 1.0f - occHF * (1.0f - closedGainHF);
    }

    void acousticAmbientSource(const vec &loc, float dist, float &volf, float &gainhf, float &reverbSend, AcousticSourceInfo &info)
    {
        info = AcousticSourceInfo();
        info.apparent = loc;
        if(!soundacoustics || !camera1)
            return;
        acousticHudSource(reverbSend);
        if(dist <= 1.0f)
            return;
        // Fixed world emitters must be blocked by their own source-to-listener path.
        float occlusion = soundpointocclusion(loc, camera1->o, [](const vec &start, const vec &direction, float length) {
            float hit = raycube(start, direction, length, RAY_POLY);
            float streamed = soundworldhit(start, direction, hit >= 0 ? min(hit, length) : length, float(WORLD_BLOCK_SIZE), [](const ivec &point) {
                int bottom;
                return sampleworldsolid(point, bottom);
            });
            return hit < 0 ? streamed : min(hit, streamed);
        });
        applyDirectOcclusion(occlusion, volf, gainhf);
        info.occlusion = clamp(occlusion * soundacousticocclusion, 0.0f, 1.0f);
        if(occlusion <= 0)
            return;
        AcousticAStarResult path;
        if(!findAcousticAStarPath(loc, dist, occlusion, path, true))
            return;
        acousticDebugPath = path.points;
        acousticDebugVirtualSource = path.virtualPosition;
        acousticDebugPathMillis = totalmillis;
        info.apparent = path.virtualPosition;
        info.virtualGain = clamp(0.18f + path.occlusion * 0.35f + path.complexity * 0.25f, 0.0f, 0.75f);
        info.virtualGainHF = clamp(gainhf * (1 - path.complexity * 0.5f), 0.02f, 1.0f);
        info.path = true;
        if(acousticReady)
            reverbSend = max(reverbSend, clamp(acousticProbe.reverbGain * soundacousticreverb * (0.35f + path.complexity * 0.25f), 0.0f, 1.0f));
    }

    void acousticSource(const vec &loc, float dist, float &volf, float &gainhf, float &reverbSend, AcousticSourceInfo *info)
    {
        if(info)
        {
            info->apparent = loc;
            info->occlusion = info->virtualGain = 0.0f;
            info->virtualGainHF = 1.0f;
            info->path = false;
        }

        if(!soundacoustics || !camera1)
            return;

        // The listener's room reverberates even when the source is nearby or directly visible.
        acousticHudSource(reverbSend);
        if(dist <= 1.0f)
            return;

        float directOcclusion = acousticDirectPointOcclusion(loc, camera1->o);
        if(directOcclusion <= 0.0f)
            return; // Cheap direct test first

        applyDirectOcclusion(directOcclusion, volf, gainhf);
        if(info)
            info->occlusion = clamp(directOcclusion * soundacousticocclusion, 0.0f, 1.0f);

        AcousticAStarResult path;
        if(!findAcousticAStarPath(loc, dist, directOcclusion, path))
            return;

        acousticDebugPath = path.points;
        acousticDebugVirtualSource = path.virtualPosition;
        acousticDebugPathMillis = totalmillis;

        if(acousticReady)
            reverbSend = max(reverbSend, clamp(acousticProbe.reverbGain * soundacousticreverb * (0.35f + path.complexity * 0.25f), 0.0f, 1.0f));

        if(info)
        {
            info->apparent = path.virtualPosition;
            info->occlusion = path.occlusion;
            info->virtualGain = clamp(0.18f + path.occlusion * 0.35f + path.complexity * 0.25f, 0.0f, 0.75f);
            info->virtualGainHF = 1.0f;
            info->path = true;
        }
    }

    void acousticHudSource(float &reverbSend)
    {
        if(!soundacoustics || !acousticReady)
            return;
        reverbSend = max(reverbSend, clamp(acousticProbe.reverbGain * soundacousticreverb, 0.0f, 1.0f));
    }

    static void drawAcousticDebugLine(const vec &a, const vec &b, const bvec &color, uchar alpha)
    {
        gle::attrib(a);
        gle::attrib(color, alpha);
        gle::attrib(b);
        gle::attrib(color, alpha);
    }

    static void drawAcousticAStarDebug(int maxradius)
    {
        if(acousticDebugPath.empty() || !camera1 || totalmillis - acousticDebugPathMillis > 500)
            return;

        vec feet = camera1->feetpos();
        int lines = 2;
        loopv(acousticDebugPath)
        {
            vec cur = acousticDebugPath[i];
            cur.z = feet.z;
            if(cur.dist(feet) <= maxradius)
                lines += 2 + (i ? 1 : 0);
        }
        if(lines <= 0)
            return;

        GLfloat oldwidth = 1.0f;
        glGetFloatv(GL_LINE_WIDTH, &oldwidth);
        glLineWidth(4.0f);
        gle::begin(GL_LINES, lines * 2);

        bvec blue(48, 128, 255);
        float marker = max(float(soundacousticastarcellsize), 8.0f) * 0.30f;
        vec virtualSource = acousticDebugVirtualSource;
        virtualSource.z = feet.z;
        drawAcousticDebugLine(vec(virtualSource.x - marker, virtualSource.y, virtualSource.z),
                              vec(virtualSource.x + marker, virtualSource.y, virtualSource.z), blue, 255);
        drawAcousticDebugLine(vec(virtualSource.x, virtualSource.y - marker, virtualSource.z),
                              vec(virtualSource.x, virtualSource.y + marker, virtualSource.z), blue, 255);

        loopv(acousticDebugPath)
        {
            vec cur = acousticDebugPath[i];
            cur.z = feet.z;
            if(cur.dist(feet) > maxradius)
                continue;
            drawAcousticDebugLine(vec(cur.x - marker * 0.45f, cur.y, cur.z), vec(cur.x + marker * 0.45f, cur.y, cur.z), blue, 230);
            drawAcousticDebugLine(vec(cur.x, cur.y - marker * 0.45f, cur.z), vec(cur.x, cur.y + marker * 0.45f, cur.z), blue, 230);
            if(i)
            {
                vec prev = acousticDebugPath[i - 1];
                prev.z = feet.z;
                drawAcousticDebugLine(prev, cur, blue, 255);
            }
        }

        xtraverts += gle::end();
        glLineWidth(oldwidth);
    }

    static void drawAcousticAStarGridDebug(int maxradius)
    {
        if(!camera1)
            return;

        float cellsize = max(float(soundacousticastarcellsize), 8.0f), radius = float(maxradius);
        if(soundacousticastarrange > 0)
            radius = min(radius, float(soundacousticastarrange));
        if(radius <= 0)
            return;

        vec feet = camera1->feetpos();
        int minx = int(floorf((feet.x - radius) / cellsize)), maxx = int(floorf((feet.x + radius) / cellsize)),
            miny = int(floorf((feet.y - radius) / cellsize)), maxy = int(floorf((feet.y + radius) / cellsize));
        int visible = 0;
        for(int y = miny; y <= maxy; ++y)
            for(int x = minx; x <= maxx; ++x)
            {
                vec center = acousticAStarCellCenter2D(ivec(x, y, 0), feet.z, cellsize);
                if(center.dist(feet) <= radius)
                    visible++;
            }
        int lines = visible * 4;
        if(lines <= 0)
            return;

        bvec white(255, 255, 255), red(255, 48, 48);
        gle::begin(GL_LINES, lines * 2);
        for(int y = miny; y <= maxy; ++y)
            for(int x = minx; x <= maxx; ++x)
            {
                ivec coord(x, y, 0);
                vec center = acousticAStarCellCenter2D(coord, feet.z, cellsize);
                if(center.dist(feet) > radius)
                    continue;
                vec probe = acousticAStarCellCenter2D(coord, camera1->o.z, cellsize),
                    v[4] = {vec(x * cellsize, y * cellsize, feet.z), vec((x + 1) * cellsize, y * cellsize, feet.z),
                            vec((x + 1) * cellsize, (y + 1) * cellsize, feet.z), vec(x * cellsize, (y + 1) * cellsize, feet.z)};
                bool passable = acousticAStarPointPassable(probe, cellsize);
                const bvec &color = passable ? white : red;
                uchar alpha = passable ? 80 : 190;
                drawAcousticDebugLine(v[0], v[1], color, alpha);
                drawAcousticDebugLine(v[1], v[2], color, alpha);
                drawAcousticDebugLine(v[2], v[3], color, alpha);
                drawAcousticDebugLine(v[3], v[0], color, alpha);
            }
        xtraverts += gle::end();
    }

    void drawAcousticsDebug()
    {
        if(!debugsoundacoustics || !soundacoustics || !camera1)
            return;
        ldrnotextureshader->set();
        GLboolean cull = glIsEnabled(GL_CULL_FACE);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);
        gle::defvertex();
        gle::defcolor(4, GL_UNSIGNED_BYTE);
        if(debugsoundacoustics == 1 || debugsoundacoustics == 3)
        {
            gle::begin(GL_LINES, acousticSamples.length() * 2);
            loopv(acousticSamples)
            {
                const AcousticRaySample &sample = acousticSamples[i];
                vec end = vec(sample.direction).mul(min(sample.distance, float(debugsoundacousticsradius))).add(sample.origin);
                drawAcousticDebugLine(sample.origin, end, sample.distance < acousticSampleRange * 0.98f ? bvec(255, 96, 48) : bvec(64, 192, 255),
                                      120);
            }
            xtraverts += gle::end();
        }
        if(debugsoundacoustics == 2 || debugsoundacoustics == 3)
        {
            drawAcousticAStarGridDebug(debugsoundacousticsradius);
            drawAcousticAStarDebug(debugsoundacousticsradius);
        }
        glDepthMask(GL_TRUE);
        if(cull)
            glEnable(GL_CULL_FACE);
    }
} // namespace acoustics
