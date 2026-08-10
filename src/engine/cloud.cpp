// Kastenbrot clouds: flat single-layer renderer v1.3 (no Cube hashtable iteration macros)
// Kastenbrot single-layer voxel clouds.
//
// Deliberately simple: one deterministic 2D cloud mask extruded into a thin voxel slab.
// No 3D noise, no cloud chunks, no pseudo raymarch. The cloud deck is one contiguous VBO
// centered around the camera with a rebuild margin. Internal faces are never generated,
// top/bottom and side faces are greedily merged, so geometry stays tiny even at long range.

#include "engine.h"
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

// general
VARP(clouds, 0, 0, 1);
VARP(cloudcellsize, 16, 128, 256);
VARP(clouddistance, 512, 16384, 32768);
VARP(cloudrebuildmargin, 1, 8, 64);
VARP(cloudupdateinterval, 1000, 60000, 600000);
VARP(cloudsmoothpasses, 0, 2, 4);
VARP(cloudsmoothkeep, 0, 3, 8);
VARP(cloudsmoothfill, 0, 5, 8);

// main cloud deck
VARP(cloudbaseheight, 0, 6144, 16384);
VARP(cloudheight, 16, 128, 1024);
FVARP(cloudscale, 0.00005f, 0.00090f, 0.05f);
FVARP(cloudcoverage, 0.0f, 0.50f, 1.0f);

// weather map
FVARP(weatherscale, 0.00001f, 0.00010f, 0.01f);
FVARP(weathercoverage, 0.0f, 0.52f, 1.0f);
FVARP(cloudweatherinfluence, 0.0f, 0.30f, 1.0f);
FVARP(weatherwindspeed, 0.0f, 0.20f, 16.0f);
FVARP(cloudspacing, 0.0f, 0.08f, 0.25f);

// wind
FVARP(cloudwindspeed, 0.0f, 1.20f, 64.0f);
FVARP(cloudwindangle, 0.0f, 18.0f, 360.0f);

// lighting: defaults intentionally minimize hard cube-face shading
FVARP(cloudambient, 0.0f, 0.82f, 4.0f);
FVARP(cloudsunlight, 0.0f, 0.78f, 4.0f);
FVARP(cloudlightwrap, 0.0f, 0.62f, 1.0f);
FVARP(cloudfacecontrast, 0.0f, 0.24f, 1.0f);
FVARP(cloudrounding, 0.0f, 0.42f, 1.0f);
FVARP(cloudrimlight, 0.0f, 0.08f, 1.0f);
FVARP(cloudundersidedarkness, 0.0f, 0.12f, 0.75f);

// opacity
FVARP(cloudalpha, 0.0f, 0.75f, 1.0f);

//post blur
VARP(cloudpostblur, 0, 0, 1);
FVARP(cloudrenderscale, 0.25f, 0.75f, 1.0f);
VARP(cloudblurradius, 0, 1, 4);
FVARP(cloudblursigma, 0.25f, 0.85f, 4.0f);

// CSM
VARP(cloudshadows, 0, 1, 1);
VARP(cloudshadowdistance, 0, 4096, 16384);

// time-of-day colours
CVARP(clouddaycolor, 0xFFFDFC);
CVARP(cloudsunsetcolor, 0xFFD2B8);
CVARP(cloudnightcolor, 0x303B58);

namespace
{
    enum
    {
        CLOUD_EMPTY = 0,
        CLOUD_FAIR
    };

    struct cloudvert
    {
        vec pos;
        bvec4 normal;
    };

    struct cloudlayer
    {
        GLuint vbo;
        int numverts, originx, originy, size, centerx, centery, settingsversion, weatherversion;
        bool built;

        cloudlayer()
            : vbo(0), numverts(0), originx(0), originy(0), size(0), centerx(INT_MIN), centery(INT_MIN),
              settingsversion(0), weatherversion(0), built(false)
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

    static cloudlayer cloudstate;
    static FastNoiseLite weathernoise, cloudnoise;
    static int noiseseed = INT_MIN, currentsettingsversion = 0, currentweatherversion = 0, lastcloudframe = -1;
    static uint currentsettingshash = 0;
    static vec cloudwind(0, 0, 0), weatherwind(0, 0, 0), cloudworldorigin(0, 0, 0);

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
        addhash(hash, uint(clouddistance));
        addhash(hash, uint(cloudrebuildmargin));
        addhash(hash, uint(cloudsmoothpasses));
        addhash(hash, uint(cloudsmoothkeep));
        addhash(hash, uint(cloudsmoothfill));
        addhash(hash, uint(cloudbaseheight));
        addhash(hash, uint(cloudheight));
        addhash(hash, hashfloat(cloudscale));
        addhash(hash, hashfloat(cloudcoverage));
        addhash(hash, hashfloat(weatherscale));
        addhash(hash, hashfloat(weathercoverage));
        addhash(hash, hashfloat(cloudweatherinfluence));
        addhash(hash, hashfloat(weatherwindspeed));
        addhash(hash, hashfloat(cloudwindangle));
        return hash;
    }

    static void setupcloudnoise(int seed)
    {
        noiseseed = seed;

        weathernoise.SetSeed(int(mixhash(uint(seed) ^ 0xA341316CU)));
        weathernoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
        weathernoise.SetFractalType(FastNoiseLite::FractalType_FBm);
        weathernoise.SetFractalOctaves(3);
        weathernoise.SetFractalGain(0.48f);
        weathernoise.SetFrequency(weatherscale);

        cloudnoise.SetSeed(int(mixhash(uint(seed) ^ 0xC8013EA4U)));
        cloudnoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
        cloudnoise.SetFractalType(FastNoiseLite::FractalType_FBm);
        cloudnoise.SetFractalOctaves(3);
        cloudnoise.SetFractalGain(0.46f);
        cloudnoise.SetFrequency(cloudscale);
    }

    static float sampleweatheractual(float x, float y)
    {
        const float weather = 0.5f + 0.5f * weathernoise.GetNoise(x - weatherwind.x, y - weatherwind.y);
        return clamp(weather + (weathercoverage - 0.5f) * 0.8f, 0.0f, 1.0f);
    }

    static bool rawcloudcell(float cloudspacex, float cloudspacey, float weather)
    {
        const float shape = 0.5f + 0.5f * cloudnoise.GetNoise(cloudspacex, cloudspacey);

        // High coverage lowers the threshold. Weather shifts it smoothly over very large regions, giving clear sky in one place and overcast sky elsewhere.
        float threshold = 0.82f + cloudspacing - cloudcoverage * 0.45f - (weather - 0.5f) * cloudweatherinfluence;
        threshold = clamp(threshold, 0.28f, 0.88f);
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

    static void meshcloudlayer(const cloudmask &mask, vector<cloudvert> &verts)
    {
        const int size = mask.size;
        const float cell = float(cloudcellsize);
        const float z0 = float(cloudbaseheight);
        const float z1 = z0 + cloudheight;

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

            const float x0 = x * cell;
            const float y0 = y * cell;
            const float x1 = (x + width) * cell;
            const float y1 = (y + height) * cell;
            addtopquad(verts, x0, y0, x1, y1, z1);
            addbottomquad(verts, x0, y0, x1, y1, z0);

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
                addxsidequad(verts, (x + 1) * cell, y * cell, (y + run) * cell, z0, z1, 1);
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
                addxsidequad(verts, x * cell, y * cell, (y + run) * cell, z0, z1, -1);
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
                addysidequad(verts, (y + 1) * cell, x * cell, (x + run) * cell, z0, z1, 1);
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
                addysidequad(verts, y * cell, x * cell, (x + run) * cell, z0, z1, -1);
                x += run;
            }
        }
    }

    static void clearcloudlayer(cloudlayer &layer)
    {
        if(layer.vbo) glDeleteBuffers_(1, &layer.vbo);
        layer.vbo = 0;
        layer.numverts = 0;
        layer.size = 0;
        layer.built = false;
        layer.centerx = layer.centery = INT_MIN;
    }

    static void buildcloudlayer(int centerx, int centery)
    {
        cloudlayer &layer = cloudstate;
        const int radius = max(int(ceilf(float(clouddistance) / max(float(cloudcellsize), 1.0f))) + cloudrebuildmargin + 2, 2);
        const int size = radius * 2 + 1;
        const int originx = centerx - radius;
        const int originy = centery - radius;

        cloudmask mask(size);

        // Sample one-cell border as well so visible border faces know about occupancy immediately outside the generated mesh. This removes artificial mesh seams.
        for(int y = -1; y <= size; ++y) for(int x = -1; x <= size; ++x)
        {
            const float cloudspacex = (originx + x + 0.5f) * cloudcellsize;
            const float cloudspacey = (originy + y + 0.5f) * cloudcellsize;
            const float actualx = cloudspacex + cloudwind.x;
            const float actualy = cloudspacey + cloudwind.y;
            const float weather = sampleweatheractual(actualx, actualy);
            mask.cell(x, y) = rawcloudcell(cloudspacex, cloudspacey, weather) ? CLOUD_FAIR : CLOUD_EMPTY;
        }

        smoothcloudmask(mask);

        vector<cloudvert> verts;
        meshcloudlayer(mask, verts);

        if(!layer.vbo) glGenBuffers_(1, &layer.vbo);
        gle::bindvbo(layer.vbo);
        glBufferData_(GL_ARRAY_BUFFER, verts.length() * sizeof(cloudvert), verts.empty() ? NULL : verts.getbuf(), GL_STATIC_DRAW);
        gle::clearvbo();

        layer.numverts = verts.length();
        layer.originx = originx;
        layer.originy = originy;
        layer.size = size;
        layer.centerx = centerx;
        layer.centery = centery;
        layer.settingsversion = currentsettingsversion;
        layer.weatherversion = currentweatherversion;
        layer.built = true;
    }

    static vec cloudrenderoffset(const cloudlayer &layer)
    {
        const double ox = double(layer.originx) * cloudcellsize;
        const double oy = double(layer.originy) * cloudcellsize;
        return vec(float(ox + cloudwind.x - cloudworldorigin.x), float(oy + cloudwind.y - cloudworldorigin.y), 0.0f);
    }

    static bool cloudlayerbounds(const cloudlayer &layer, vec &center, float &radius, float distance)
    {
        if(!layer.built || !layer.vbo || !layer.numverts || !layer.size) return false;
        const float span = layer.size * cloudcellsize;
        const float halfspan = span * 0.5f;
        const float z0 = float(cloudbaseheight);
        const float z1 = z0 + cloudheight;
        const float halfheight = (z1 - z0) * 0.5f;
        center = cloudrenderoffset(layer).add(vec(halfspan, halfspan, z0 + halfheight));
        radius = sqrtf(2.0f * halfspan * halfspan + halfheight * halfheight);
        const float dx = center.x - camera1->o.x;
        const float dy = center.y - camera1->o.y;
        return dx * dx + dy * dy <= (distance + radius) * (distance + radius);
    }

    static void enablecloudvertexformat(const cloudlayer &layer)
    {
        gle::bindvbo(layer.vbo);
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
        gle::clearvbo();
    }

    static void setclouduniforms(Shader *shader)
    {
        shader->set();

        const float day = clamp((sunlightscale - 0.08f) / 0.72f, 0.0f, 1.0f);
        const float sunset = 4.0f * day * (1.0f - day);

        LOCALPARAM(cloudsundir, sunlightdir);
        LOCALPARAM(cloudsuncolor, vec(sunlight.tocolor()).mul(sunlightscale));
        LOCALPARAM(cloudambientcolor, vec(ambient.tocolor()).mul(ambientscale));
        LOCALPARAM(cloudcamera, camera1->o);
        LOCALPARAM(clouddaytint, clouddaycolor.tocolor());
        LOCALPARAM(cloudsunsettint, cloudsunsetcolor.tocolor());
        LOCALPARAM(cloudnighttint, cloudnightcolor.tocolor());
        LOCALPARAMF(cloudlighting, cloudambient, cloudsunlight, cloudundersidedarkness, 0.0f);
        LOCALPARAMF(cloudappearance, cloudlightwrap, cloudfacecontrast, cloudrounding, cloudrimlight);
        LOCALPARAMF(cloudtime, day, sunset, 2.0f * ldrscale, cloudalpha);
    }

    static int drawcloudgeometry()
    {
        Shader *shader = lookupshaderbyname("cloud");
        if(!shader || !camera1) return 0;

        vec center;
        float radius;
        if(!cloudlayerbounds(cloudstate, center, radius, float(clouddistance))) return 0;
        if(isvisiblesphere(radius, center) == VFC_NOT_VISIBLE) return 0;

        setclouduniforms(shader);
        enablecloudvertexformat(cloudstate);
        LOCALPARAM(cloudmeshoffset, cloudrenderoffset(cloudstate));
        glDrawArrays(GL_TRIANGLES, 0, cloudstate.numverts);
        glde++;
        const int renderedverts = cloudstate.numverts;
        disablecloudvertexformat();
        return renderedverts;
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
    lastcloudframe = lastmillis;

    const int seed = game::getworldseed();
    const uint settingshash = cloudsettingshash(seed);
    if(settingshash != currentsettingshash || seed != noiseseed)
    {
        currentsettingshash = settingshash;
        ++currentsettingsversion;
        setupcloudnoise(seed);
        cloudstate.built = false;
    }

    const float seconds = game::environment::gettimemillis() / 1000.0f;
    const float angle = cloudwindangle * RAD;
    const vec direction(cosf(angle), sinf(angle), 0.0f);
    cloudwind = vec(direction).mul(cloudwindspeed * seconds);

    const int weatherstep = game::environment::gettimemillis() / max(cloudupdateinterval, 1);
    const float weatherseconds = weatherstep * cloudupdateinterval / 1000.0f;
    weatherwind = vec(direction).mul(weatherwindspeed * weatherseconds);
    currentweatherversion = weatherstep;

    vec absolute = camera1->o;
    worldpositiontoabsolute(absolute);
    cloudworldorigin = vec(absolute).sub(camera1->o);

    const int centerx = int(floorf((absolute.x - cloudwind.x) / max(float(cloudcellsize), 1.0f)));
    const int centery = int(floorf((absolute.y - cloudwind.y) / max(float(cloudcellsize), 1.0f)));
    const int margin = max(cloudrebuildmargin, 1);

    const bool moved = cloudstate.centerx == INT_MIN || abs(centerx - cloudstate.centerx) >= margin || abs(centery - cloudstate.centery) >= margin;
    const bool stale = !cloudstate.built || cloudstate.settingsversion != currentsettingsversion || cloudstate.weatherversion != currentweatherversion;
    if(moved || stale) buildcloudlayer(centerx, centery);
}

void renderclouds()
{
    if(!clouds || !camera1) return;

    GLint oldfb = 0;
    GLint oldviewport[4] = { 0, 0, screenw, screenh };
    GLint olddepthfunc = GL_LESS;
    GLint oldsrc = GL_ONE, olddst = GL_ZERO;
    GLint oldcullmode = GL_BACK, oldfrontface = GL_CCW, oldactivetex = GL_TEXTURE0;
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
    int renderedverts = 0;

    if(!cloudpostblur || cloudblurradius <= 0)
    {
        glBindFramebuffer_(GL_FRAMEBUFFER, GLuint(oldfb));
        glViewport(oldviewport[0], oldviewport[1], oldviewport[2], oldviewport[3]);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);

        if(cloudalpha < 0.999f)
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        else glDisable(GL_BLEND);

        renderedverts = drawcloudgeometry();
    }
    else
    {
        const int rtw = max(int(ceilf(oldviewport[2] * cloudrenderscale)), 1);
        const int rth = max(int(ceilf(oldviewport[3] * cloudrenderscale)), 1);

        if(setupcloudtarget(rtw, rth))
        {
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
            glDepthFunc(GL_LESS);
            glDepthMask(GL_TRUE);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glFrontFace(GL_CCW);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

            renderedverts = drawcloudgeometry();
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
            glBindFramebuffer_(GL_FRAMEBUFFER, GLuint(oldfb));
            glViewport(oldviewport[0], oldviewport[1], oldviewport[2], oldviewport[3]);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_FALSE);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glFrontFace(GL_CCW);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            renderedverts = drawcloudgeometry();
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
    glActiveTexture_(GLenum(oldactivetex));

    if(oldblend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if(olddepthtest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if(oldcull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if(oldscissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if(oldstencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if(oldpolyoffset) glEnable(GL_POLYGON_OFFSET_FILL); else glDisable(GL_POLYGON_OFFSET_FILL);

    if(renderedverts) xtraverts += renderedverts;
}

void rendercloudshadows(int split)
{
    if(!clouds || !cloudshadows || !cloudshadowdistance || !camera1) return;
    const float direct = clamp((sunlightscale - 0.06f) / 0.34f, 0.0f, 1.0f);
    if(direct <= 0.03f) return;

    Shader *shader = lookupshaderbyname("cloudshadow");
    if(!shader) return;
    shader->set();

    cloudlayer &layer = cloudstate;

    vec center;
    float radius;
    if(!cloudlayerbounds(layer, center, radius, float(cloudshadowdistance))) return;

    const vec offset = cloudrenderoffset(layer);
    const float span = layer.size * cloudcellsize;
    const int z0 = cloudbaseheight;
    const int z1 = z0 + cloudheight;
    const ivec bbmin(int(floorf(offset.x)), int(floorf(offset.y)), z0);
    const ivec bbmax(int(ceilf(offset.x + span)), int(ceilf(offset.y + span)), z1);
    if(!(calcbbcsmsplits(bbmin, bbmax) & (1 << split))) return;

    enablecloudvertexformat(layer);
    LOCALPARAM(cloudmeshoffset, offset);
    glDrawArrays(GL_TRIANGLES, 0, layer.numverts);
    glde++;
    disablecloudvertexformat();
}

void cleanupclouds()
{
    clearcloudlayer(cloudstate);
    cleanupcloudtarget();
    noiseseed = INT_MIN;
    currentsettingshash = 0;
    currentsettingsversion = currentweatherversion = 0;
    lastcloudframe = -1;
}
