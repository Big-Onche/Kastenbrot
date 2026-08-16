// Kastenbrot clouds: flat single-layer renderer streamed as deterministic world-space tiles.

#include "engine.h"
#include "weather.h"
#ifdef SQRT3
#pragma push_macro("SQRT3")
#undef SQRT3
#define RESTORE_CLOUD_SQRT3
#endif
#include "FastNoiseLite.h"
#ifdef RESTORE_CLOUD_SQRT3
#pragma pop_macro("SQRT3")
#undef RESTORE_CLOUD_SQRT3
#endif

namespace game
{
    extern int getworldseed();

    namespace environment
    {
        extern int gettimemillis();
    }
}

extern bvec ambient, sunlight;
extern float ambientscale, sunlightscale;
extern vec sunlightdir;
extern int atmo;
extern float atmoplanetsize, atmoheight, atmobright, atmosunlightscale, atmosundisksize, atmosundiskcorona, atmohaze, atmodensity, atmoozone,
             hdrgamma;
extern bvec atmosunlight;

// general
VARP(clouds, 0, 1, 1);
VARP(cloudcellsize, 16, 128, 256);
VARP(clouddistance, 512, 32768, 32768);
VARP(cloudrebuildmargin, 1, 8, 64);
VARP(cloudsmoothpasses, 0, 2, 4);
VARP(cloudsmoothkeep, 0, 3, 8);
VARP(cloudsmoothfill, 0, 5, 8);

// main cloud deck
VARP(cloudbaseheight, 0, 6144, 16384);
VARP(cloudheight, 16, 192, 1024);
FVARP(clouddome, 0.0f, 0.07f, 2.0f);
FVARP(cloudscale, 0.00005f, 0.00090f, 0.05f);
FVARP(cloudspacing, 0.0f, 0.08f, 0.25f);

// wind
FVARP(cloudwindspeed, 0.0f, 16.0f, 64.0f);
FVARP(cloudwindangle, 0.0f, 18.0f, 360.0f);

// lighting: rounded side shading keeps the voxel silhouette while avoiding flat, uniformly lit slabs
// cloudambient and cloudsunlight are the sky-light and sun-light strength controls used by the raymarched lighting model
FVARP(cloudambient, 0.0f, 1.0f, 4.0f);
FVARP(cloudsunlight, 0.0f, 1.5f, 4.0f);
FVARP(cloudlightwrap, 0.0f, 0.55f, 1.0f);
FVARP(cloudfacecontrast, 0.0f, 0.15f, 1.0f);
FVARP(cloudrounding, 0.0f, 0.72f, 1.0f);
FVARP(cloudrimlight, 0.0f, 0.5f, 1.0f);
FVARP(cloudundersidedarkness, 0.0f, 0.17f, 0.75f);

// fixed-cost interior depth and sunlight visibility sampling
FVARP(cloudraymarchdepth, 0.25f, 1.5f, 8.0f);
VARP(cloudraymarchsteps, 1, 8, 16);
VARP(cloudsunmarchsteps, 2, 8, 16);
FVARP(cloudselfshadow, 0.0f, 0.5f, 1.0f);

// finite-distance atmospheric extinction and in-scattering; driven by the same physical controls as the sky atmosphere
FVARP(cloudscatterstrength, 0.0f, 2.0f, 4.0f);
FVARP(cloudscatterblue, 0.0f, 1.5f, 4.0f);

// opacity
FVARP(cloudalpha, 0.0f, 0.6f, 1.0f);

// cheap depth fog while the camera occupies a cloud cell
VAR(cloudinsidefog, 0, 1, 1);
FVAR(cloudinsidefogdistance, 16.0f, 96.0f, 2048.0f);
FVAR(cloudinsidefogopacity, 0.0f, 0.92f, 1.0f);
FVAR(cloudinsidefogfade, 0.0f, 6.0f, 256.0f);

// post blur
VARP(cloudpostblur, 0, 0, 1);
FVARP(cloudrenderscale, 0.25f, 1.0f, 1.0f);
VARP(cloudblurradius, 0, 1, 4);
FVARP(cloudblursigma, 0.25f, 0.85f, 4.0f);

// dedicated projected cloud shadows
VARP(cloudshadows, 0, 1, 1);
VARP(cloudshadowdistance, 0, 4096, 16384);
FVARP(cloudshadowalpha, 0.0f, 0.32f, 1.0f);
VARP(cloudshadowmapsize, 64, 1024, 2048);
FVARP(cloudshadowsoftness, 0.0f, 1.25f, 8.0f);

// time-of-day colours
CVARP(clouddaycolor, 0xFFFDFC);
CVARP(cloudsunsetcolor, 0xFFD2B8);
CVARP(cloudnightcolor, 0x303B58);

namespace
{
    enum
    {
        CLOUD_EMPTY = 0,
        CLOUD_FAIR,
        CLOUD_TILE_CELLS = 64,
        CLOUD_TILE_MASK_CELLS = CLOUD_TILE_CELLS + 2,
        CLOUD_TILE_GENERATION_BUDGET = 2,
        CLOUD_TILE_HYSTERESIS = 1
    };

    struct cloudvert
    {
        vec pos;
        bvec4 normal;
    };

    struct cloudface
    {
        vec center;
        float depth;
        int firstvert;

        cloudface() : center(0, 0, 0), depth(0.0f), firstvert(0) {}
        cloudface(const vec &center, int firstvert) : center(center), depth(0.0f), firstvert(firstvert) {}
    };

    struct cloudtile
    {
        vector<cloudface> faces;
        vector<uchar> occupancy;
        GLuint vbo, ebo, masktex;
        int numverts, tx, ty, settingsversion;
        bool built;

        cloudtile(int tx, int ty)
            : vbo(0), ebo(0), masktex(0), numverts(0), tx(tx), ty(ty), settingsversion(0), built(false)
        {
        }
    };

    struct cloudmask
    {
        vector<uchar> cells;
        int size, stride;

        cloudmask(int size) : size(size), stride(size + 2)
        {
            const int total = stride * stride;
            cells.growbuf(total);
            loopi(total) cells.add(CLOUD_EMPTY);
        }

        int get(int x, int y) const
        {
            if(x < -1 || y < -1 || x > size || y > size) return CLOUD_EMPTY;
            return cells[(y + 1) * stride + x + 1];
        }

        uchar &cell(int x, int y)
        {
            return cells[(y + 1) * stride + x + 1];
        }
    };

    struct cloudtilecandidate
    {
        int tx, ty;
        double distance;

        cloudtilecandidate() : tx(0), ty(0), distance(0.0) {}
        cloudtilecandidate(int tx, int ty, double distance) : tx(tx), ty(ty), distance(distance) {}
    };

    static bool sortcloudtilecandidates(const cloudtilecandidate &a, const cloudtilecandidate &b)
    {
        if(a.distance != b.distance) return a.distance < b.distance;
        if(a.ty != b.ty) return a.ty < b.ty;
        return a.tx < b.tx;
    }

    static vector<cloudtile *> cloudtiles;
    static FastNoiseLite cloudnoise;
    static int noiseseed = INT_MIN, currentsettingsversion = 0, currentweathersettingsversion = 0, lastcloudframe = -1;
    static int cloudtilesetversion = 0;
    static uint currentsettingshash = 0;
    static vec cloudwind(0, 0, 0), cloudworldorigin(0, 0, 0);
    static GLuint cloudscenefbo = 0, cloudscenetex = 0;
    static int cloudscenew = 0, cloudsceneh = 0;
    static float cloudscreenx = 0.0f, cloudscreeny = 0.0f, cloudscreenw = 1.0f, cloudscreenh = 1.0f, cloudrenderalpha = 1.0f;

    static float cloudinsidepenetration();

    static uint mixhash(uint value)
    {
        value ^= value >> 16;
        value *= 0x7FEB352DU;
        value ^= value >> 15;
        value *= 0x846CA68BU;
        value ^= value >> 16;
        return value;
    }

    static uint hashfloat(float value)
    {
        union
        {
            float f;
            uint i;
        } bits;
        bits.f = value;
        return bits.i;
    }

    static void addhash(uint &hash, uint value)
    {
        hash = mixhash(hash ^ value);
    }

    static uint cloudsettingshash(int seed)
    {
        uint hash = mixhash(uint(seed));
        addhash(hash, uint(cloudcellsize));
        addhash(hash, uint(cloudsmoothpasses));
        addhash(hash, uint(cloudsmoothkeep));
        addhash(hash, uint(cloudsmoothfill));
        addhash(hash, uint(cloudbaseheight));
        addhash(hash, uint(cloudheight));
        addhash(hash, hashfloat(clouddome));
        addhash(hash, hashfloat(cloudscale));
        addhash(hash, hashfloat(cloudspacing));
        return hash;
    }

    static void setupcloudnoise(int seed)
    {
        noiseseed = seed;

        cloudnoise.SetSeed(int(mixhash(uint(seed) ^ 0xC8013EA4U)));
        cloudnoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
        cloudnoise.SetFractalType(FastNoiseLite::FractalType_FBm);
        cloudnoise.SetFractalOctaves(3);
        cloudnoise.SetFractalGain(0.46f);
        cloudnoise.SetFrequency(cloudscale);
    }

    static bool rawcloudcell(float cloudspacex, float cloudspacey, float cloudcoverage)
    {
        const float shape = 0.5f + 0.5f * cloudnoise.GetNoise(cloudspacex, cloudspacey);
        const float threshold = clamp(1.0f - cloudcoverage + cloudspacing, 0.0f, 1.0f);
        return shape > threshold;
    }

    static void smoothcloudmask(cloudmask &mask)
    {
        if(cloudsmoothpasses <= 0) return;

        const int total = mask.stride * mask.stride;
        vector<uchar> next;
        next.growbuf(total);
        loopi(total) next.add(CLOUD_EMPTY);

        for(int pass = 0; pass < cloudsmoothpasses; ++pass)
        {
            memcpy(next.getbuf(), mask.cells.getbuf(), total * sizeof(uchar));
            for(int y = 0; y < mask.size; ++y) for(int x = 0; x < mask.size; ++x)
            {
                int neighbors = 0;
                for(int oy = -1; oy <= 1; ++oy) for(int ox = -1; ox <= 1; ++ox)
                {
                    if(!ox && !oy) continue;
                    if(mask.get(x + ox, y + oy) != CLOUD_EMPTY) ++neighbors;
                }
                const bool current = mask.get(x, y) != CLOUD_EMPTY;
                const bool filled = current ? neighbors >= cloudsmoothkeep : neighbors >= cloudsmoothfill;
                next[(y + 1) * mask.stride + x + 1] = filled ? CLOUD_FAIR : CLOUD_EMPTY;
            }
            memcpy(mask.cells.getbuf(), next.getbuf(), total * sizeof(uchar));
        }
    }

    static void addcloudvertex(vector<cloudvert> &verts, const vec &pos, int nx, int ny, int nz)
    {
        cloudvert &vert = verts.add();
        vert.pos = pos;
        vert.normal = bvec4(uchar((nx + 1) * 127), uchar((ny + 1) * 127), uchar((nz + 1) * 127), 255);
    }

    static void addquad(vector<cloudvert> &verts, const vec &a, const vec &b, const vec &c, const vec &d,
                        int nx, int ny, int nz)
    {
        addcloudvertex(verts, a, nx, ny, nz);
        addcloudvertex(verts, b, nx, ny, nz);
        addcloudvertex(verts, c, nx, ny, nz);
        addcloudvertex(verts, a, nx, ny, nz);
        addcloudvertex(verts, c, nx, ny, nz);
        addcloudvertex(verts, d, nx, ny, nz);
    }

    static void addtopquad(vector<cloudvert> &verts, float x0, float y0, float x1, float y1, float z)
    {
        addquad(verts, vec(x0, y0, z), vec(x1, y0, z), vec(x1, y1, z), vec(x0, y1, z), 0, 0, 1);
    }

    static void addbottomquad(vector<cloudvert> &verts, float x0, float y0, float x1, float y1, float z)
    {
        addquad(verts, vec(x0, y1, z), vec(x1, y1, z), vec(x1, y0, z), vec(x0, y0, z), 0, 0, -1);
    }

    static void addxsidequad(vector<cloudvert> &verts, float x, float y0, float y1, float z0, float z1, int sign)
    {
        if(sign > 0)
            addquad(verts, vec(x, y0, z0), vec(x, y1, z0), vec(x, y1, z1), vec(x, y0, z1), 1, 0, 0);
        else
            addquad(verts, vec(x, y1, z0), vec(x, y0, z0), vec(x, y0, z1), vec(x, y1, z1), -1, 0, 0);
    }

    static void addysidequad(vector<cloudvert> &verts, float y, float x0, float x1, float z0, float z1, int sign)
    {
        if(sign > 0)
            addquad(verts, vec(x1, y, z0), vec(x0, y, z0), vec(x0, y, z1), vec(x1, y, z1), 0, 1, 0);
        else
            addquad(verts, vec(x0, y, z0), vec(x1, y, z0), vec(x1, y, z1), vec(x0, y, z1), 0, -1, 0);
    }

    static void meshcloudtile(const cloudmask &mask, vector<cloudvert> &verts)
    {
        ZoneScopedN("Clouds/MeshTile");
        const int size = mask.size;
        const float cell = float(cloudcellsize);
        const float z0 = float(cloudbaseheight);
        const float z1 = z0 + cloudheight;
        const int meshstep = clouddome > 0.0f ? 4 : size;

        // Greedy top/bottom rectangles.
        vector<uchar> topmask;
        topmask.growbuf(size * size);
        for(int y = 0; y < size; ++y) for(int x = 0; x < size; ++x) topmask.add(uchar(mask.get(x, y)));

        for(int y = 0; y < size; ++y) for(int x = 0; x < size;)
        {
            const int type = topmask[y * size + x];
            if(type == CLOUD_EMPTY)
            {
                ++x;
                continue;
            }

            int width = 1;
            while(x + width < size && topmask[y * size + x + width] != CLOUD_EMPTY) ++width;

            int height = 1;
            bool extend = true;
            while(y + height < size && extend)
            {
                for(int i = 0; i < width; ++i) if(topmask[(y + height) * size + x + i] == CLOUD_EMPTY)
                {
                    extend = false;
                    break;
                }
                if(extend) ++height;
            }

            for(int row = 0; row < height; row += meshstep) for(int col = 0; col < width; col += meshstep)
            {
                const float x0 = (x + col) * cell, y0 = (y + row) * cell;
                const float x1 = (x + min(col + meshstep, width)) * cell;
                const float y1 = (y + min(row + meshstep, height)) * cell;
                addtopquad(verts, x0, y0, x1, y1, z1);
                addbottomquad(verts, x0, y0, x1, y1, z0);
            }

            for(int row = 0; row < height; ++row) for(int col = 0; col < width; ++col)
                topmask[(y + row) * size + x + col] = CLOUD_EMPTY;
            x += width;
        }

        // +/- X borders, merged along Y.
        for(int x = 0; x < size; ++x)
        {
            int y = 0;
            while(y < size)
            {
                if(mask.get(x, y) == CLOUD_EMPTY || mask.get(x + 1, y) != CLOUD_EMPTY)
                {
                    ++y;
                    continue;
                }
                int run = 1;
                while(y + run < size && mask.get(x, y + run) != CLOUD_EMPTY && mask.get(x + 1, y + run) == CLOUD_EMPTY) ++run;
                for(int offset = 0; offset < run; offset += meshstep)
                    addxsidequad(verts, (x + 1) * cell, (y + offset) * cell, (y + min(offset + meshstep, run)) * cell, z0, z1, 1);
                y += run;
            }

            y = 0;
            while(y < size)
            {
                if(mask.get(x, y) == CLOUD_EMPTY || mask.get(x - 1, y) != CLOUD_EMPTY)
                {
                    ++y;
                    continue;
                }
                int run = 1;
                while(y + run < size && mask.get(x, y + run) != CLOUD_EMPTY && mask.get(x - 1, y + run) == CLOUD_EMPTY) ++run;
                for(int offset = 0; offset < run; offset += meshstep)
                    addxsidequad(verts, x * cell, (y + offset) * cell, (y + min(offset + meshstep, run)) * cell, z0, z1, -1);
                y += run;
            }
        }

        // +/- Y borders, merged along X.
        for(int y = 0; y < size; ++y)
        {
            int x = 0;
            while(x < size)
            {
                if(mask.get(x, y) == CLOUD_EMPTY || mask.get(x, y + 1) != CLOUD_EMPTY)
                {
                    ++x;
                    continue;
                }
                int run = 1;
                while(x + run < size && mask.get(x + run, y) != CLOUD_EMPTY && mask.get(x + run, y + 1) == CLOUD_EMPTY) ++run;
                for(int offset = 0; offset < run; offset += meshstep)
                    addysidequad(verts, (y + 1) * cell, (x + offset) * cell, (x + min(offset + meshstep, run)) * cell, z0, z1, 1);
                x += run;
            }

            x = 0;
            while(x < size)
            {
                if(mask.get(x, y) == CLOUD_EMPTY || mask.get(x, y - 1) != CLOUD_EMPTY)
                {
                    ++x;
                    continue;
                }
                int run = 1;
                while(x + run < size && mask.get(x + run, y) != CLOUD_EMPTY && mask.get(x + run, y - 1) == CLOUD_EMPTY) ++run;
                for(int offset = 0; offset < run; offset += meshstep)
                    addysidequad(verts, y * cell, (x + offset) * cell, (x + min(offset + meshstep, run)) * cell, z0, z1, -1);
                x += run;
            }
        }
    }

    static void clearcloudtile(cloudtile &tile)
    {
        if(tile.vbo) glDeleteBuffers_(1, &tile.vbo);
        if(tile.ebo) glDeleteBuffers_(1, &tile.ebo);
        if(tile.masktex) glDeleteTextures(1, &tile.masktex);
        tile.vbo = tile.ebo = 0;
        tile.masktex = 0;
        tile.faces.shrink(0);
        tile.occupancy.shrink(0);
        tile.numverts = 0;
        tile.built = false;
    }

    static void clearcloudtiles()
    {
        loopv(cloudtiles)
        {
            clearcloudtile(*cloudtiles[i]);
            delete cloudtiles[i];
        }
        cloudtiles.shrink(0);
        ++cloudtilesetversion;
    }

    static cloudtile *findcloudtile(int tx, int ty)
    {
        loopv(cloudtiles) if(cloudtiles[i]->tx == tx && cloudtiles[i]->ty == ty) return cloudtiles[i];
        return NULL;
    }

    static void buildcloudtile(cloudtile &tile)
    {
        {ZoneScopedN("Clouds/GenerateTile");}
        const int halo = cloudsmoothpasses + 1;
        const int masksize = CLOUD_TILE_CELLS + 2 * halo;
        const int origincellx = tile.tx * CLOUD_TILE_CELLS;
        const int origincelly = tile.ty * CLOUD_TILE_CELLS;

        cloudmask expanded(masksize);

        // The smoothing halo is generated from absolute cell coordinates. Independently generated neighboring tiles therefore agree exactly at
        // their borders.
        for(int y = -1; y <= masksize; ++y) for(int x = -1; x <= masksize; ++x)
        {
            const float cloudspacex = (origincellx + x - halo + 0.5f) * cloudcellsize;
            const float cloudspacey = (origincelly + y - halo + 0.5f) * cloudcellsize;
            const float cloudcoverage = game::weather::samplecoverage(cloudspacex, cloudspacey);
            expanded.cell(x, y) = rawcloudcell(cloudspacex, cloudspacey, cloudcoverage) ? CLOUD_FAIR : CLOUD_EMPTY;
        }

        smoothcloudmask(expanded);

        cloudmask mask(CLOUD_TILE_CELLS);
        for(int y = -1; y <= CLOUD_TILE_CELLS; ++y) for(int x = -1; x <= CLOUD_TILE_CELLS; ++x)
            mask.cell(x, y) = expanded.get(x + halo, y + halo);

        vector<cloudvert> verts;
        meshcloudtile(mask, verts);
        ASSERT(verts.length() % 6 == 0);

        tile.occupancy.shrink(0);
        tile.occupancy.growbuf(CLOUD_TILE_MASK_CELLS * CLOUD_TILE_MASK_CELLS);
        for(int y = -1; y <= CLOUD_TILE_CELLS; ++y) for(int x = -1; x <= CLOUD_TILE_CELLS; ++x)
            tile.occupancy.add(mask.get(x, y) != CLOUD_EMPTY ? 255 : 0);

        ZoneScopedN("Clouds/UploadTile");
        GLint oldactive = GL_TEXTURE0, oldtex = 0, oldunpack = 4;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &oldactive);
        glActiveTexture_(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldtex);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldunpack);
        if(!tile.masktex) glGenTextures(1, &tile.masktex);
        glBindTexture(GL_TEXTURE_2D, tile.masktex);
        // Marches use the hardware-filtered mask for fractional edge density. Topology reads remain exact because they sample texel centers.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, hasTRG ? GL_R8 : GL_LUMINANCE8, CLOUD_TILE_MASK_CELLS, CLOUD_TILE_MASK_CELLS, 0,
                     hasTRG ? GL_RED : GL_LUMINANCE, GL_UNSIGNED_BYTE, tile.occupancy.getbuf());
        glPixelStorei(GL_UNPACK_ALIGNMENT, oldunpack);
        glBindTexture(GL_TEXTURE_2D, GLuint(oldtex));
        glActiveTexture_(GLenum(oldactive));

        if(!tile.vbo) glGenBuffers_(1, &tile.vbo);
        if(!tile.ebo) glGenBuffers_(1, &tile.ebo);
        gle::bindvbo(tile.vbo);
        glBufferData_(GL_ARRAY_BUFFER, verts.length() * sizeof(cloudvert), verts.empty() ? NULL : verts.getbuf(), GL_STATIC_DRAW);
        gle::clearvbo();

        tile.faces.shrink(0);
        tile.faces.growbuf(verts.length() / 6);
        for(int firstvert = 0; firstvert < verts.length(); firstvert += 6)
        {
            vec center = vec(verts[firstvert].pos).add(verts[firstvert + 1].pos).add(verts[firstvert + 2].pos)
                                                    .add(verts[firstvert + 5].pos).mul(0.25f);
            tile.faces.add(cloudface(center, firstvert));
        }

        tile.numverts = verts.length();
        tile.settingsversion = currentsettingsversion;
        tile.built = true;
        ++cloudtilesetversion;
    }

    static vec cloudrenderoffset(const cloudtile &tile)
    {
        const double ox = double(tile.tx) * CLOUD_TILE_CELLS * cloudcellsize;
        const double oy = double(tile.ty) * CLOUD_TILE_CELLS * cloudcellsize;
        return vec(float(ox + cloudwind.x - cloudworldorigin.x), float(oy + cloudwind.y - cloudworldorigin.y), 0.0f);
    }

    static bool cloudtilebounds(const cloudtile &tile, vec &center, float &radius, float distance)
    {
        if(!tile.built || !tile.vbo || !tile.numverts) return false;
        const float span = CLOUD_TILE_CELLS * cloudcellsize;
        const float halfspan = span * 0.5f;
        const float z0 = float(cloudbaseheight);
        const float z1 = z0 + cloudheight;
        const vec offset = cloudrenderoffset(tile);
        const float centerx = offset.x + halfspan, centery = offset.y + halfspan;
        const float dx = centerx - camera1->o.x, dy = centery - camera1->o.y;
        const float domecoefficient = clouddome * max(z0, 0.0f) / max(float(clouddistance * clouddistance), 1.0f);
        const float farthestdistance = sqrtf(dx * dx + dy * dy) + SQRT2 * halfspan;
        const float maxdomedrop = domecoefficient * farthestdistance * farthestdistance;
        const float halfheight = (z1 - (z0 - maxdomedrop)) * 0.5f;
        center = vec(centerx, centery, z1 - halfheight);
        radius = sqrtf(2.0f * halfspan * halfspan + halfheight * halfheight);
        return dx * dx + dy * dy <= (distance + radius) * (distance + radius);
    }

    static void enablecloudvertexformat(const cloudtile &tile)
    {
        gle::bindvbo(tile.vbo);
        const cloudvert *pointer = 0;
        gle::vertexpointer(sizeof(cloudvert), pointer->pos.v);
        gle::normalpointer(sizeof(cloudvert), pointer->normal.v, GL_UNSIGNED_BYTE, 4);
        gle::enablevertex();
        gle::enablenormal();
    }

    static void disablecloudvertexformat()
    {
        gle::disablevertex();
        gle::disablenormal();
        gle::clearebo();
        gle::clearvbo();
    }

    static bool sortcloudfaces(const cloudface &a, const cloudface &b)
    {
        return a.depth > b.depth;
    }

    static void updatecloudfaceorder(cloudtile &tile)
    {
        const vec offset = cloudrenderoffset(tile);
        loopv(tile.faces)
        {
            const vec worldcenter = vec(tile.faces[i].center).add(offset);
            tile.faces[i].depth = vec(worldcenter).sub(camera1->o).dot(camdir);
        }
        tile.faces.sort(sortcloudfaces);

        static vector<GLuint> indices;
        indices.shrink(0);
        indices.growbuf(tile.numverts);
        loopv(tile.faces) loopk(6) indices.add(GLuint(tile.faces[i].firstvert + k));

        gle::bindebo(tile.ebo);
        glBufferData_(GL_ELEMENT_ARRAY_BUFFER, indices.length() * sizeof(GLuint), indices.empty() ? NULL : indices.getbuf(), GL_STREAM_DRAW);
        gle::clearebo();
    }

    static void setclouduniforms(Shader *shader, const cloudtile &tile)
    {
        shader->set();

        const float day = clamp((sunlightscale - 0.08f) / 0.72f, 0.0f, 1.0f);
        const float sunset = 4.0f * day * (1.0f - day);
        const float cell = max(float(cloudcellsize), 1.0f);
        const float span = max(CLOUD_TILE_MASK_CELLS * cell, 1.0f);
        const float domecoefficient = clouddome * max(float(cloudbaseheight), 0.0f) / max(float(clouddistance * clouddistance), 1.0f);
        const vec offset = vec(cloudrenderoffset(tile)).sub(vec(cell, cell, 0.0f));

        const float earthradius = 6371e3f, earthairheight = 8.4e3f, earthhazeheight = 1.25e3f;
        const float planetradius = earthradius * atmoplanetsize;
        const float gm = max(0.95f - 0.2f * atmohaze, 0.65f);
        const float miescale = pow((1 - gm) * (1 - gm) / (4 * M_PI), -2.0f / 3.0f);
        const float mieangle = cosf(0.5f * atmosundisksize * (1 - atmosundiskcorona) * RAD);
        static const vec lambda(680e-9f, 550e-9f, 450e-9f), k(0.686f, 0.678f, 0.666f), ozone(3.426f, 8.298f, 0.356f);
        const vec betar = vec(lambda).square().square().recip().mul(1.241e-30f / M_LN2 * atmodensity);
        const vec betam = vec(lambda).recip().square().mul(k).mul(9.072e-17f / M_LN2 * atmohaze);
        const vec betao = vec(ozone).mul(1.5e-7f / M_LN2 * atmoozone);
        vec atmosuncolor = !atmosunlight.iszero() ? atmosunlight.tocolor() : vec(1.0f, 0.98f, 0.92f);
        atmosuncolor.mul(atmosunlightscale);
        const vec atmosunscale = vec(atmosuncolor).mul(ldrscale).pow(hdrgamma).mul(atmobright * 16);

        LOCALPARAM(cloudsundir, sunlightdir);
        LOCALPARAM(cloudsuncolor, vec(sunlight.tocolor()).mul(sunlightscale));
        LOCALPARAM(cloudambientcolor, vec(ambient.tocolor()).mul(ambientscale));
        LOCALPARAM(cloudcamera, camera1->o);
        LOCALPARAM(clouddaytint, clouddaycolor.tocolor());
        LOCALPARAM(cloudsunsettint, cloudsunsetcolor.tocolor());
        LOCALPARAM(cloudnighttint, cloudnightcolor.tocolor());
        LOCALPARAMF(cloudlighting, cloudambient, cloudsunlight, cloudundersidedarkness, cloudinsidepenetration() >= 0.0f ? 1.0f : 0.0f);
        LOCALPARAMF(cloudappearance, cloudlightwrap, cloudfacecontrast, cloudrounding, cloudrimlight);
        LOCALPARAMF(cloudgeometry, float(cloudbaseheight), float(cloudbaseheight + cloudheight), 1.0f / max(float(cloudheight), 1.0f), cell);
        LOCALPARAMF(cloudvolume, offset.x, offset.y, 1.0f / span, span);
        LOCALPARAMF(clouddomeparams, domecoefficient, 0.0f, 0.0f, 0.0f);
        LOCALPARAMF(cloudraymarch, cloudraymarchdepth * cell, float(cloudraymarchsteps), float(cloudsunmarchsteps), cloudselfshadow);
        LOCALPARAMF(atmosphereparams, planetradius, 1 + 100e3f * atmoheight / planetradius, earthairheight * atmoheight / planetradius,
                    earthhazeheight * atmoheight / planetradius);
        LOCALPARAMF(ozoneparams, 25e3f * atmoheight / planetradius, 15e3f * atmoheight / planetradius);
        LOCALPARAMF(mieparams, miescale * (1 + gm * gm), miescale * -2 * gm, mieangle);
        LOCALPARAM(betarayleigh, betar);
        LOCALPARAM(betamie, betam);
        LOCALPARAM(betaozone, betao);
        LOCALPARAM(atmospheresunlight, atmosunscale);
        LOCALPARAMF(cloudscatterparams, atmo ? cloudscatterstrength : 0.0f, cloudscatterblue, 0.0f, 0.0f);
        LOCALPARAMF(cloudscreenparams, cloudscreenx, cloudscreeny, 1.0f / max(cloudscreenw, 1.0f), 1.0f / max(cloudscreenh, 1.0f));
        LOCALPARAMF(cloudtime, day, sunset, 2.0f * ldrscale, cloudrenderalpha);
    }

    static int drawcloudgeometry(bool sorted = false)
    {
        Shader *shader = lookupshaderbyname("cloud");
        if(!shader || !camera1) return 0;

        glActiveTexture_(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, cloudscenetex);
        glActiveTexture_(GL_TEXTURE0);
        int renderedverts = 0;
        loopv(cloudtiles)
        {
            cloudtile &tile = *cloudtiles[i];
            vec center;
            float radius;
            if(!cloudtilebounds(tile, center, radius, float(clouddistance))) continue;
            if(isvisiblesphere(radius, center) == VFC_NOT_VISIBLE) continue;

            if(sorted) updatecloudfaceorder(tile);
            glBindTexture(GL_TEXTURE_2D, tile.masktex);
            setclouduniforms(shader, tile);
            enablecloudvertexformat(tile);
            LOCALPARAM(cloudmeshoffset, cloudrenderoffset(tile));
            if(sorted)
            {
                gle::bindebo(tile.ebo);
                glDrawElements(GL_TRIANGLES, tile.numverts, GL_UNSIGNED_INT, 0);
            }
            else glDrawArrays(GL_TRIANGLES, 0, tile.numverts);
            glde++;
            renderedverts += tile.numverts;
            disablecloudvertexformat();
        }
        return renderedverts;
    }

    static int drawcloudsurface(GLenum depthfunc)
    {
        if(cloudalpha <= 0.0f) return 0;

        glDepthFunc(depthfunc);

        // cloudalpha is resolved in the shader against the captured scene. Geometry itself remains opaque and writes depth normally.
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        return drawcloudgeometry();
    }

    static void cleanupcloudscene()
    {
        if(cloudscenefbo) glDeleteFramebuffers_(1, &cloudscenefbo);
        if(cloudscenetex) glDeleteTextures(1, &cloudscenetex);
        cloudscenefbo = cloudscenetex = 0;
        cloudscenew = cloudsceneh = 0;
    }

    static bool setupcloudscene(int w, int h)
    {
        w = max(w, 1);
        h = max(h, 1);
        if(cloudscenefbo && cloudscenetex && cloudscenew == w && cloudsceneh == h) return true;

        cleanupcloudscene();
        cloudscenew = w;
        cloudsceneh = h;

        glGenTextures(1, &cloudscenetex);
        glBindTexture(GL_TEXTURE_2D, cloudscenetex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, NULL);

        glGenFramebuffers_(1, &cloudscenefbo);
        glBindFramebuffer_(GL_FRAMEBUFFER, cloudscenefbo);
        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cloudscenetex, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        const bool complete = glCheckFramebufferStatus_(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer_(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        if(!complete) cleanupcloudscene();
        return complete;
    }

    static bool capturecloudscene(GLuint sourcefb, const GLint viewport[4])
    {
        if(!setupcloudscene(viewport[2], viewport[3])) return false;

        glBindFramebuffer_(GL_READ_FRAMEBUFFER, sourcefb);
        glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, cloudscenefbo);
        glBlitFramebuffer_(viewport[0], viewport[1], viewport[0] + viewport[2], viewport[1] + viewport[3], 0, 0, viewport[2], viewport[3],
                           GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer_(GL_FRAMEBUFFER, sourcefb);
        return true;
    }

    // optional post-process target
    static GLuint cloudfbo[2] = { 0, 0 }, cloudtex[2] = { 0, 0 }, clouddepthrb = 0;
    static int cloudrtw = 0, cloudrth = 0;

    static void cleanupcloudtarget()
    {
        if(cloudfbo[0] || cloudfbo[1]) glDeleteFramebuffers_(2, cloudfbo);
        if(cloudtex[0] || cloudtex[1]) glDeleteTextures(2, cloudtex);
        if(clouddepthrb) glDeleteRenderbuffers_(1, &clouddepthrb);
        cloudfbo[0] = cloudfbo[1] = 0;
        cloudtex[0] = cloudtex[1] = 0;
        clouddepthrb = 0;
        cloudrtw = cloudrth = 0;
    }

    static bool setupcloudtarget(int w, int h)
    {
        w = max(w, 1);
        h = max(h, 1);
        if(cloudfbo[0] && cloudfbo[1] && cloudrtw == w && cloudrth == h) return true;

        cleanupcloudtarget();
        cloudrtw = w;
        cloudrth = h;

        glGenFramebuffers_(2, cloudfbo);
        glGenTextures(2, cloudtex);
        for(int i = 0; i < 2; ++i)
        {
            glBindTexture(GL_TEXTURE_2D, cloudtex[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, NULL);

            glBindFramebuffer_(GL_FRAMEBUFFER, cloudfbo[i]);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cloudtex[i], 0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
        }

        glGenRenderbuffers_(1, &clouddepthrb);
        glBindRenderbuffer_(GL_RENDERBUFFER, clouddepthrb);
        glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glBindFramebuffer_(GL_FRAMEBUFFER, cloudfbo[0]);
        glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, clouddepthrb);

        const bool complete0 = glCheckFramebufferStatus_(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer_(GL_FRAMEBUFFER, cloudfbo[1]);
        const bool complete1 = glCheckFramebufferStatus_(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

        glBindFramebuffer_(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindRenderbuffer_(GL_RENDERBUFFER, 0);

        if(!complete0 || !complete1)
        {
            cleanupcloudtarget();
            return false;
        }
        return true;
    }

    static void drawcloudscreenquad()
    {
        gle::defvertex(2);
        gle::begin(GL_TRIANGLE_STRIP);
        gle::attribf(-1.0f, -1.0f);
        gle::attribf( 1.0f, -1.0f);
        gle::attribf(-1.0f,  1.0f);
        gle::attribf( 1.0f,  1.0f);
        gle::end();
    }

    static bool cloudcelloccupied(const cloudtile &tile, int x, int y)
    {
        return x >= 0 && y >= 0 && x < CLOUD_TILE_CELLS && y < CLOUD_TILE_CELLS &&
               tile.occupancy[(y + 1) * CLOUD_TILE_MASK_CELLS + x + 1] != 0;
    }

    static int floordiv(int value, int divisor)
    {
        int result = value / divisor;
        if(value < 0 && value % divisor) --result;
        return result;
    }

    static bool cloudcelloccupied(int cellx, int celly)
    {
        const int tx = floordiv(cellx, CLOUD_TILE_CELLS), ty = floordiv(celly, CLOUD_TILE_CELLS);
        const cloudtile *tile = findcloudtile(tx, ty);
        return tile && tile->built && cloudcelloccupied(*tile, cellx - tx * CLOUD_TILE_CELLS, celly - ty * CLOUD_TILE_CELLS);
    }

    static float cloudinsidepenetration()
    {
        if(!clouds || !camera1) return -1.0f;

        const float z0 = float(cloudbaseheight), z1 = z0 + cloudheight;
        if(camera1->o.z <= z0 || camera1->o.z >= z1) return -1.0f;

        const float cell = max(float(cloudcellsize), 1.0f);
        vec absolute = camera1->o;
        worldpositiontoabsolute(absolute);
        const double gridx = (double(absolute.x) - cloudwind.x) / cell, gridy = (double(absolute.y) - cloudwind.y) / cell;
        const int x = int(floor(gridx)), y = int(floor(gridy));
        if(!cloudcelloccupied(x, y)) return -1.0f;

        float surfacedistance = min(camera1->o.z - z0, z1 - camera1->o.z);
        const float localx = float(gridx - x) * cell, localy = float(gridy - y) * cell;
        if(!cloudcelloccupied(x - 1, y)) surfacedistance = min(surfacedistance, localx);
        if(!cloudcelloccupied(x + 1, y)) surfacedistance = min(surfacedistance, cell - localx);
        if(!cloudcelloccupied(x, y - 1)) surfacedistance = min(surfacedistance, localy);
        if(!cloudcelloccupied(x, y + 1)) surfacedistance = min(surfacedistance, cell - localy);

        return surfacedistance;
    }

    static float cloudinsideamount()
    {
        if(!cloudinsidefog || cloudinsidefogopacity <= 0.0f) return 0.0f;

        const float penetration = cloudinsidepenetration();
        if(penetration < 0.0f) return 0.0f;
        return cloudinsidefogfade > 0.0f ? clamp(penetration / cloudinsidefogfade, 0.0f, 1.0f) : 1.0f;
    }


    // Dedicated cloud shadow mask.
    // The mask is rendered in cloud-local XY space, so cloud wind/camera translation only changes the sampling transform and does not force a mask
    // rerender every frame.
    static GLuint cloudshadowfbo[2] = { 0, 0 }, cloudshadowtex[2] = { 0, 0 };
    static int cloudshadowrt = 0;
    static int cloudshadowmasktilesetversion = -1;
    static int cloudshadowmaskmapsize = 0;
    static float cloudshadowmasksoftness = -1.0f;
    static vec cloudshadoworigin(0, 0, 0);
    static float cloudshadowspanx = 1.0f, cloudshadowspany = 1.0f;

    static void cleanupcloudshadowtarget()
    {
        if(cloudshadowfbo[0] || cloudshadowfbo[1]) glDeleteFramebuffers_(2, cloudshadowfbo);
        if(cloudshadowtex[0] || cloudshadowtex[1]) glDeleteTextures(2, cloudshadowtex);
        cloudshadowfbo[0] = cloudshadowfbo[1] = 0;
        cloudshadowtex[0] = cloudshadowtex[1] = 0;
        cloudshadowrt = 0;
        cloudshadowmasktilesetversion = -1;
        cloudshadowmaskmapsize = 0;
        cloudshadowmasksoftness = -1.0f;
    }

    static bool setupcloudshadowtarget()
    {
        const int size = clamp(cloudshadowmapsize, 64, 2048);
        if(cloudshadowfbo[0] && cloudshadowfbo[1] && cloudshadowrt == size) return true;

        cleanupcloudshadowtarget();
        cloudshadowrt = size;

        glGenFramebuffers_(2, cloudshadowfbo);
        glGenTextures(2, cloudshadowtex);

        for(int i = 0; i < 2; ++i)
        {
            glBindTexture(GL_TEXTURE_2D, cloudshadowtex[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cloudshadowrt, cloudshadowrt, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

            glBindFramebuffer_(GL_FRAMEBUFFER, cloudshadowfbo[i]);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cloudshadowtex[i], 0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);

            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            {
                glBindFramebuffer_(GL_FRAMEBUFFER, 0);
                glBindTexture(GL_TEXTURE_2D, 0);
                cleanupcloudshadowtarget();
                return false;
            }
        }

        glBindFramebuffer_(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        return true;
    }

    static void cloudshadowblurweights(float weights[5])
    {
        if(cloudshadowsoftness <= 0.0f)
        {
            weights[0] = 1.0f;
            for(int i = 1; i < 5; ++i) weights[i] = 0.0f;
            return;
        }

        const int radius = clamp(int(ceilf(cloudshadowsoftness)), 1, 4);
        const float sigma = max(cloudshadowsoftness * 0.65f, 0.35f);
        float total = 0.0f;

        for(int i = 0; i < 5; ++i)
        {
            weights[i] = i > radius ? 0.0f : expf(-float(i * i) / (2.0f * sigma * sigma));
            total += i ? weights[i] * 2.0f : weights[i];
        }

        if(total <= 0.0f) total = 1.0f;
        for(int i = 0; i < 5; ++i) weights[i] /= total;
    }

    static void blurcloudshadowtarget()
    {
        if(cloudshadowsoftness <= 0.0f || !cloudshadowfbo[0] || !cloudshadowfbo[1]) return;

        Shader *shader = lookupshaderbyname("cloudshadowblur");
        if(!shader) return;

        float weights[5];
        cloudshadowblurweights(weights);

        // Above 4.0, spread the fixed 9-tap kernel farther apart instead of adding
        // more taps. This keeps the cost fixed while still allowing very soft shadows.
        const float spread = max(cloudshadowsoftness / 4.0f, 1.0f);
        const float texel = spread / max(float(cloudshadowrt), 1.0f);

        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        shader->set();
        LOCALPARAMF(cloudshadowblurweights0, weights[0], weights[1], weights[2], weights[3]);
        LOCALPARAMF(cloudshadowblurweights1, weights[4], 0, 0, 0);

        glBindFramebuffer_(GL_FRAMEBUFFER, cloudshadowfbo[1]);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glViewport(0, 0, cloudshadowrt, cloudshadowrt);
        glBindTexture(GL_TEXTURE_2D, cloudshadowtex[0]);
        LOCALPARAMF(cloudshadowblurdir, texel, 0, 0, 0);
        drawcloudscreenquad();

        glBindFramebuffer_(GL_FRAMEBUFFER, cloudshadowfbo[0]);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glBindTexture(GL_TEXTURE_2D, cloudshadowtex[1]);
        LOCALPARAMF(cloudshadowblurdir, 0, texel, 0, 0);
        drawcloudscreenquad();
    }

    static bool updatecloudshadowmask()
    {
        if(!cloudshadows || cloudshadowalpha <= 0.0f || cloudtiles.empty()) return false;
        if(!setupcloudshadowtarget()) return false;

        const bool stale =
            cloudshadowmasktilesetversion != cloudtilesetversion ||
            cloudshadowmaskmapsize != cloudshadowmapsize ||
            cloudshadowmasksoftness != cloudshadowsoftness;

        if(!stale) return cloudshadowtex[0] != 0;

        glBindFramebuffer_(GL_FRAMEBUFFER, cloudshadowfbo[0]);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glViewport(0, 0, cloudshadowrt, cloudshadowrt);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);

        bool anygeometry = false;
        float minx = FLT_MAX, miny = FLT_MAX, maxx = -FLT_MAX, maxy = -FLT_MAX;
        const float tilespan = CLOUD_TILE_CELLS * float(cloudcellsize);
        loopv(cloudtiles) if(cloudtiles[i]->built && cloudtiles[i]->numverts)
        {
            const float tilex = float(double(cloudtiles[i]->tx) * CLOUD_TILE_CELLS * cloudcellsize);
            const float tiley = float(double(cloudtiles[i]->ty) * CLOUD_TILE_CELLS * cloudcellsize);
            minx = min(minx, tilex);
            miny = min(miny, tiley);
            maxx = max(maxx, tilex + tilespan);
            maxy = max(maxy, tiley + tilespan);
            anygeometry = true;
        }

        if(anygeometry)
        {
            Shader *shader = lookupshaderbyname("cloudshadowmask");
            if(!shader) return false;

            shader->set();
            cloudshadoworigin = vec(minx, miny, float(cloudbaseheight));
            cloudshadowspanx = max(maxx - minx, 1.0f);
            cloudshadowspany = max(maxy - miny, 1.0f);
            loopv(cloudtiles)
            {
                cloudtile &tile = *cloudtiles[i];
                if(!tile.built || !tile.numverts) continue;
                const float tilex = float(double(tile.tx) * CLOUD_TILE_CELLS * cloudcellsize);
                const float tiley = float(double(tile.ty) * CLOUD_TILE_CELLS * cloudcellsize);
                LOCALPARAMF(cloudshadowmaskparams, 1.0f / cloudshadowspanx, 1.0f / cloudshadowspany,
                            (tilex - minx) / cloudshadowspanx, (tiley - miny) / cloudshadowspany);
                enablecloudvertexformat(tile);
                glDrawArrays(GL_TRIANGLES, 0, tile.numverts);
                glde++;
                disablecloudvertexformat();
            }

            blurcloudshadowtarget();
        }

        cloudshadowmasktilesetversion = cloudtilesetversion;
        cloudshadowmaskmapsize = cloudshadowmapsize;
        cloudshadowmasksoftness = cloudshadowsoftness;
        return true;
    }

    static void drawcloudshadowoverlay(GLuint framebuffer, const GLint viewport[4])
    {
        if(!cloudshadows || cloudshadowalpha <= 0.0f || !cloudshadowdistance || !camera1) return;

        const float direct = clamp((sunlightscale - 0.06f) / 0.34f, 0.0f, 1.0f);
        if(direct <= 0.03f) return;
        if(sunlightdir.z <= 0.01f) return;
        if(!updatecloudshadowmask() || !cloudshadowtex[0]) return;

        Shader *shader = lookupshaderbyname("cloudshadowoverlay");
        if(!shader) return;

        glBindFramebuffer_(GL_FRAMEBUFFER, framebuffer);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        // Multiplicative blend: the shader outputs 1.0 in lit pixels and a value
        // below 1.0 under clouds. This leaves the existing opaque lighting intact
        // except for the cloud attenuation.
        glEnable(GL_BLEND);
        glBlendFunc(GL_ZERO, GL_SRC_COLOR);

        shader->set();

        // Wind and world-origin shifts translate the cached topology without rerendering the mask.
        vec shadoworigin(float(double(cloudshadoworigin.x) + cloudwind.x - cloudworldorigin.x),
                         float(double(cloudshadoworigin.y) + cloudwind.y - cloudworldorigin.y), cloudshadoworigin.z);
        LOCALPARAM(cloudshadoworigin, shadoworigin);
        LOCALPARAM(cloudsundir, sunlightdir);
        LOCALPARAM(camera, camera1->o);
        LOCALPARAMF(cloudshadowparams, 1.0f / cloudshadowspanx, cloudshadowalpha * direct, float(cloudshadowdistance),
                    1.0f / cloudshadowspany);

        glActiveTexture_(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, cloudshadowtex[0]);

        // Tesseract/Cube's standard g-buffer depth binding; cloudshadowoverlay uses
        // the same gfetch/gdepthunpack path as the deferred/volumetric shaders.
        glActiveTexture_(GL_TEXTURE3);
        bindgdepth();
        glActiveTexture_(GL_TEXTURE0);

        drawcloudscreenquad();

        glBindTexture(GL_TEXTURE_2D, 0);
    }

    static void cloudblurweights(float weights[5])
    {
        const int radius = clamp(cloudblurradius, 0, 4);
        const float sigma = max(cloudblursigma, 0.01f);
        float total = 0.0f;
        for(int i = 0; i < 5; ++i)
        {
            weights[i] = i > radius ? 0.0f : expf(-float(i * i) / (2.0f * sigma * sigma));
            total += i ? weights[i] * 2.0f : weights[i];
        }
        if(total <= 0.0f)
        {
            weights[0] = 1.0f;
            for(int i = 1; i < 5; ++i) weights[i] = 0.0f;
            return;
        }
        for(int i = 0; i < 5; ++i) weights[i] /= total;
    }

    static void blurcloudtarget()
    {
        if(!cloudpostblur || cloudblurradius <= 0 || !cloudfbo[0] || !cloudfbo[1]) return;
        Shader *shader = lookupshaderbyname("cloudblur");
        if(!shader) return;

        float weights[5];
        cloudblurweights(weights);

        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);

        shader->set();
        LOCALPARAMF(cloudblurweights0, weights[0], weights[1], weights[2], weights[3]);
        LOCALPARAMF(cloudblurweights1, weights[4], 0, 0, 0);

        glBindFramebuffer_(GL_FRAMEBUFFER, cloudfbo[1]);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glViewport(0, 0, cloudrtw, cloudrth);
        glBindTexture(GL_TEXTURE_2D, cloudtex[0]);
        LOCALPARAMF(cloudblurdir, 1.0f / max(cloudrtw, 1), 0, 0, 0);
        drawcloudscreenquad();

        glBindFramebuffer_(GL_FRAMEBUFFER, cloudfbo[0]);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glBindTexture(GL_TEXTURE_2D, cloudtex[1]);
        LOCALPARAMF(cloudblurdir, 0, 1.0f / max(cloudrth, 1), 0, 0);
        drawcloudscreenquad();
    }

    static void compositecloudtarget()
    {
        if(!cloudtex[0]) return;
        Shader *shader = lookupshaderbyname("cloudcomposite");
        if(!shader) return;

        shader->set();
        glBindTexture(GL_TEXTURE_2D, cloudtex[0]);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        drawcloudscreenquad();
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void updateclouds()
{
    if(!clouds || !camera1) return;
    if(lastcloudframe == lastmillis) return;
    ZoneScopedN("Clouds/Stream");

    lastcloudframe = lastmillis;

    const int seed = game::weather::getseed(game::getworldseed());
    game::weather::update(seed);
    const uint settingshash = cloudsettingshash(seed);
    const int weathersettingsversion = game::weather::getcoverageversion();
    if(settingshash != currentsettingshash || seed != noiseseed || weathersettingsversion != currentweathersettingsversion)
    {
        currentsettingshash = settingshash;
        currentweathersettingsversion = weathersettingsversion;
        ++currentsettingsversion;
        setupcloudnoise(seed);
        clearcloudtiles();
    }

    const double weathermillis = game::weather::gettimemillis();
    const float seconds = float(weathermillis / 1000.0);
    const float angle = game::weather::getwindangle(cloudwindangle) * RAD;
    const vec direction(cosf(angle), sinf(angle), 0.0f);
    cloudwind = vec(direction).mul(game::weather::getcloudspeed(cloudwindspeed) * seconds);

    vec absolute = camera1->o;
    worldpositiontoabsolute(absolute);
    cloudworldorigin = vec(absolute).sub(camera1->o);

    const double cell = max(double(cloudcellsize), 1.0);
    const double cloudx = (double(absolute.x) - cloudwind.x) / cell;
    const double cloudy = (double(absolute.y) - cloudwind.y) / cell;
    const double radius = double(clouddistance) / cell;
    const double keepspan = radius + CLOUD_TILE_HYSTERESIS * CLOUD_TILE_CELLS;
    const int centertx = int(floor(cloudx / CLOUD_TILE_CELLS)), centerty = int(floor(cloudy / CLOUD_TILE_CELLS));
    const int tileradius = int(ceil(radius / CLOUD_TILE_CELLS)) + 1;

    loopvrev(cloudtiles)
    {
        cloudtile &tile = *cloudtiles[i];
        const double nearestx = max(double(tile.tx * CLOUD_TILE_CELLS), min(cloudx, double((tile.tx + 1) * CLOUD_TILE_CELLS)));
        const double nearesty = max(double(tile.ty * CLOUD_TILE_CELLS), min(cloudy, double((tile.ty + 1) * CLOUD_TILE_CELLS)));
        const double dx = nearestx - cloudx, dy = nearesty - cloudy;
        if(dx * dx + dy * dy <= keepspan * keepspan) continue;
        clearcloudtile(tile);
        delete cloudtiles.remove(i);
        ++cloudtilesetversion;
    }

    vector<cloudtilecandidate> missing;
    for(int ty = centerty - tileradius; ty <= centerty + tileradius; ++ty)
        for(int tx = centertx - tileradius; tx <= centertx + tileradius; ++tx)
        {
            const double nearestx = max(double(tx * CLOUD_TILE_CELLS), min(cloudx, double((tx + 1) * CLOUD_TILE_CELLS)));
            const double nearesty = max(double(ty * CLOUD_TILE_CELLS), min(cloudy, double((ty + 1) * CLOUD_TILE_CELLS)));
            const double dx = nearestx - cloudx, dy = nearesty - cloudy;
            if(dx * dx + dy * dy > radius * radius || findcloudtile(tx, ty)) continue;
            missing.add(cloudtilecandidate(tx, ty, dx * dx + dy * dy));
        }
    missing.sort(sortcloudtilecandidates);

    const int generate = min(missing.length(), int(CLOUD_TILE_GENERATION_BUDGET));
    loopi(generate)
    {
        cloudtile *tile = new cloudtile(missing[i].tx, missing[i].ty);
        cloudtiles.add(tile);
        buildcloudtile(*tile);
    }
}

void renderclouds()
{
    if(!clouds || !camera1) return;

    const bool camerainside = cloudinsidepenetration() >= 0.0f;

    GLint oldfb = 0;
    GLint oldviewport[4] = { 0, 0, screenw, screenh };
    GLint olddepthfunc = GL_LESS;
    GLint oldsrc = GL_ONE, olddst = GL_ZERO;
    GLint oldcullmode = GL_BACK, oldfrontface = GL_CCW, oldactivetex = GL_TEXTURE0, oldtex2d0 = 0, oldtex2d1 = 0;
    GLfloat oldclear[4] = { 0, 0, 0, 0 };
    GLfloat oldcleardepth = 1.0f;
    GLboolean olddepthmask = GL_TRUE;
    GLboolean oldcolormask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };

    const GLboolean oldblend = glIsEnabled(GL_BLEND);
    const GLboolean olddepthtest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean oldcull = glIsEnabled(GL_CULL_FACE);
    const GLboolean oldscissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean oldstencil = glIsEnabled(GL_STENCIL_TEST);
    const GLboolean oldpolyoffset = glIsEnabled(GL_POLYGON_OFFSET_FILL);

    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldfb);
    glGetIntegerv(GL_VIEWPORT, oldviewport);
    glGetIntegerv(GL_DEPTH_FUNC, &olddepthfunc);
    glGetIntegerv(GL_BLEND_SRC_RGB, &oldsrc);
    glGetIntegerv(GL_BLEND_DST_RGB, &olddst);
    glGetIntegerv(GL_CULL_FACE_MODE, &oldcullmode);
    glGetIntegerv(GL_FRONT_FACE, &oldfrontface);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldactivetex);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &olddepthmask);
    glGetBooleanv(GL_COLOR_WRITEMASK, oldcolormask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, oldclear);
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &oldcleardepth);

    glActiveTexture_(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldtex2d0);
    glActiveTexture_(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldtex2d1);
    glActiveTexture_(GL_TEXTURE0);
    int renderedverts = 0;

    // project the low-resolution cloud shadow mask onto the already-lit opaque scene
    drawcloudshadowoverlay(GLuint(oldfb), oldviewport);

    cloudrenderalpha = cloudalpha;
    if(cloudalpha > 0.0f && cloudalpha < 1.0f && !capturecloudscene(GLuint(oldfb), oldviewport)) cloudrenderalpha = 1.0f;

    if(!cloudpostblur || cloudblurradius <= 0)
    {
        cloudscreenx = float(oldviewport[0]);
        cloudscreeny = float(oldviewport[1]);
        cloudscreenw = float(oldviewport[2]);
        cloudscreenh = float(oldviewport[3]);
        glBindFramebuffer_(GL_FRAMEBUFFER, GLuint(oldfb));
        glViewport(oldviewport[0], oldviewport[1], oldviewport[2], oldviewport[3]);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        if(camerainside) glDisable(GL_CULL_FACE);
        else glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CW);

        renderedverts = drawcloudsurface(GL_LEQUAL);
    }
    else
    {
        const int rtw = max(int(ceilf(oldviewport[2] * cloudrenderscale)), 1);
        const int rth = max(int(ceilf(oldviewport[3] * cloudrenderscale)), 1);

        if(setupcloudtarget(rtw, rth))
        {
            cloudscreenx = cloudscreeny = 0.0f;
            cloudscreenw = float(rtw);
            cloudscreenh = float(rth);
            glBindFramebuffer_(GL_FRAMEBUFFER, cloudfbo[0]);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glViewport(0, 0, rtw, rth);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_POLYGON_OFFSET_FILL);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);
            glClearDepth(1.0);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            glEnable(GL_DEPTH_TEST);
            if(camerainside) glDisable(GL_CULL_FACE);
            else glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glFrontFace(GL_CW);

            renderedverts = drawcloudsurface(GL_LESS);
            if(renderedverts)
            {
                blurcloudtarget();
                glBindFramebuffer_(GL_FRAMEBUFFER, GLuint(oldfb));
                glViewport(oldviewport[0], oldviewport[1], oldviewport[2], oldviewport[3]);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_STENCIL_TEST);
                glDisable(GL_POLYGON_OFFSET_FILL);
                compositecloudtarget();
            }
        }
        else
        {
            cloudscreenx = float(oldviewport[0]);
            cloudscreeny = float(oldviewport[1]);
            cloudscreenw = float(oldviewport[2]);
            cloudscreenh = float(oldviewport[3]);
            glBindFramebuffer_(GL_FRAMEBUFFER, GLuint(oldfb));
            glViewport(oldviewport[0], oldviewport[1], oldviewport[2], oldviewport[3]);
            glEnable(GL_DEPTH_TEST);
            if(camerainside) glDisable(GL_CULL_FACE);
            else glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glFrontFace(GL_CW);
            renderedverts = drawcloudsurface(GL_LEQUAL);
        }
    }

    glBindFramebuffer_(GL_FRAMEBUFFER, GLuint(oldfb));
    glViewport(oldviewport[0], oldviewport[1], oldviewport[2], oldviewport[3]);
    glClearColor(oldclear[0], oldclear[1], oldclear[2], oldclear[3]);
    glClearDepth(oldcleardepth);
    glDepthFunc(GLenum(olddepthfunc));
    glDepthMask(olddepthmask);
    glColorMask(oldcolormask[0], oldcolormask[1], oldcolormask[2], oldcolormask[3]);
    glCullFace(GLenum(oldcullmode));
    glFrontFace(GLenum(oldfrontface));
    glBlendFunc(GLenum(oldsrc), GLenum(olddst));
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, GLuint(oldtex2d0));
    glActiveTexture_(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, GLuint(oldtex2d1));
    glActiveTexture_(GLenum(oldactivetex));

    if(oldblend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if(olddepthtest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if(oldcull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if(oldscissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if(oldstencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if(oldpolyoffset) glEnable(GL_POLYGON_OFFSET_FILL); else glDisable(GL_POLYGON_OFFSET_FILL);

    if(renderedverts) xtraverts += renderedverts;
}

void rendercloudfog()
{
    const float inside = cloudinsideamount();
    if(inside <= 0.0f) return;

    Shader *shader = lookupshaderbyname("cloudfog");
    if(!shader) return;

    const float day = clamp((sunlightscale - 0.08f) / 0.72f, 0.0f, 1.0f);
    const float sunset = 4.0f * day * (1.0f - day);
    vec fogcolor(cloudnightcolor.tocolor());
    fogcolor.lerp(clouddaycolor.tocolor(), day).lerp(cloudsunsetcolor.tocolor(), sunset * 0.35f).mul(ldrscale);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    shader->set();
    LOCALPARAM(camera, camera1->o);
    LOCALPARAM(cloudfogcolor, fogcolor);
    LOCALPARAMF(cloudfogparams, 1.0f / max(cloudinsidefogdistance, 1.0f), cloudinsidefogopacity * inside, 0.0f, 0.0f);

    glActiveTexture_(GL_TEXTURE3);
    bindgdepth();
    glActiveTexture_(GL_TEXTURE0);
    drawcloudscreenquad();

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
}

void rendercloudshadows(int split)
{
    // cloud shadows no longer consume the main CSM; they use their own low-resolution projected mask in renderclouds()
    (void)split;
}

void cleanupclouds()
{
    clearcloudtiles();
    cleanupcloudscene();
    cleanupcloudtarget();
    cleanupcloudshadowtarget();
    noiseseed = INT_MIN;
    game::weather::reset();
    currentsettingshash = 0;
    currentsettingsversion = currentweathersettingsversion = 0;
    lastcloudframe = -1;
}
