// Camera-local daylight and color bounce, solved on immutable worker snapshots.

#include "localambientfield.h"
#include "engine.h"
#include "worldruntime.h"
#include "localambientgeometry.h"

static void localambienttogglechanged();
static void localambientfieldchanged();
static void localambientgichanged();
static void resetlocalambientfar();

VARFP(localambient, 0, 0, 1, localambienttogglechanged());
VARF(localambientresolution, 4, 16, 128, localambientfieldchanged());
VARF(localambientmaxdist, 64, 1024, 4096, localambientfieldchanged());
VARF(localambientattenuation, 1, 4, 255, localambientfieldchanged());
FVARF(localambientverticalbias, 0, 0.25f, 1, localambientfieldchanged());
FVAR(localambientstrength, 0, 1, 1);
FVAR(localambientmin, 0, 0.04f, 1);
// Retained for old configs; the converged wavefront no longer needs GPU passes.
VAR(localambientgpupasses, 0, 0, 64);

VARFP(localambientgi, 0, 1, 1, localambientgichanged());
FVAR(localambientgiintensity, 0, 0.75, 2);
VARF(localambientgipasses, 0, 6, 32, localambientgichanged());
FVARF(localambientgidecay, 0, 0.80f, 1, localambientgichanged());
FVAR(localambientgisaturation, 0, 1.5f, 3);
FVAR(localambientgimax, 0, 0.50f, 4);
VAR(localambientcapturecells, 4096, 131072, 524288);
FVAR(localambientcapturems, 0.1f, 1.0f, 10.0f);
VAR(localambientscroll, 0, 1, 1);
VAR(localambientscrollstep, 1, 4, 32);
FVAR(localambientdeadzone, 0.1f, 0.5f, 0.9f);
VAR(localambientdebug, 0, 0, 2);
VARFP(localambientfar, 0, 1, 1, resetlocalambientfar());
VARF(localambientfarresolution, 16, 128, 1024, resetlocalambientfar());
FVAR(localambientfarms, 0.1f, 0.25f, 4.0f);

// A world-wide 2D sky-height cache is much smaller than a second lighting volume.
// Capture nearby dirty tiles first; edits and streaming only refresh touched XY.
static GLuint localambientfartexture = 0;
static int localambientfarside = 0, localambientfarspacing = 0, localambientfartiles = 0;
static int localambientfartile = -1, localambientfarrow = 0;
static vector<float> localambientfarheights;
static vector<uchar> localambientfardirty;

static void resetlocalambientfar()
{
    localambientfarside = 0;
    localambientfartile = -1;
    localambientfarheights.setsize(0);
    localambientfardirty.setsize(0);
}

static void invalidatelocalambientfar(const ivec &minimum, const ivec &maximum)
{
    if(!localambientfarside) return;
    const int span = 16 * localambientfarspacing;
    const int x0 = clamp(minimum.x / span, 0, localambientfartiles), y0 = clamp(minimum.y / span, 0, localambientfartiles),
              x1 = clamp((maximum.x + span - 1) / span, 0, localambientfartiles),
              y1 = clamp((maximum.y + span - 1) / span, 0, localambientfartiles);
    for(int y = y0; y < y1; y++) for(int x = x0; x < x1; x++) localambientfardirty[y * localambientfartiles + x] = 1;
}

static void updatelocalambientfar()
{
    if(!localambientfar) return;
    ZoneScopedN("LocalAmbient/Far sky height");
    if(!localambientfarside)
    {
        localambientfarspacing = max(localambientfarresolution, (worldsize + 1023) / 1024);
        localambientfarside = (worldsize + localambientfarspacing - 1) / localambientfarspacing;
        localambientfartiles = (localambientfarside + 15) / 16;
        localambientfarheights.pad(localambientfarside * localambientfarside);
        memset(localambientfarheights.getbuf(), 0, localambientfarheights.length() * sizeof(float));
        localambientfardirty.pad(localambientfartiles * localambientfartiles);
        memset(localambientfardirty.getbuf(), 1, localambientfardirty.length());
        if(!localambientfartexture) glGenTextures(1, &localambientfartexture);
        glActiveTexture_(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, localambientfartexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        const GLfloat border[4] = { 0, 0, 0, 0 };
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, localambientfarside, localambientfarside, 0, GL_RED, GL_FLOAT,
                     localambientfarheights.getbuf());
    }
    const Uint64 start = SDL_GetPerformanceCounter();
    const double budget = localambientfarms * SDL_GetPerformanceFrequency() / 1000.0;
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, localambientfartexture);
    do
    {
        if(localambientfartile < 0)
        {
            float nearest = 1e30f;
            loopv(localambientfardirty) if(localambientfardirty[i])
            {
                const float dx = (i % localambientfartiles * 16 + 8) * localambientfarspacing - camera1->o.x,
                            dy = (i / localambientfartiles * 16 + 8) * localambientfarspacing - camera1->o.y,
                            distance = dx * dx + dy * dy;
                if(distance < nearest) { nearest = distance; localambientfartile = i; }
            }
            if(localambientfartile < 0) break;
            // Changes received during this capture remain queued for another pass.
            localambientfardirty[localambientfartile] = 0;
            localambientfarrow = 0;
        }
        const int x = localambientfartile % localambientfartiles * 16,
                  y = localambientfartile / localambientfartiles * 16 + localambientfarrow,
                  count = min(16, localambientfarside - x);
        float *heights = localambientfarheights.getbuf() + y * localambientfarside + x;
        loopi(count) heights[i] = sampleworldskyheight((x + i) * localambientfarspacing + localambientfarspacing / 2,
                                                      y * localambientfarspacing + localambientfarspacing / 2);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, count, 1, GL_RED, GL_FLOAT, heights);
        if(++localambientfarrow >= 16 || y + 1 >= localambientfarside) localambientfartile = -1;
    }
    while(SDL_GetPerformanceCounter() - start < budget);
    glBindTexture(GL_TEXTURE_2D, 0);
}

enum
{
    LOCALAMBIENT_MAX_DIMENSION = 128,
    LOCALAMBIENT_SCROLL_REGIONS = 3
};


struct localambientjob
{
    uint serial;
    ivec origin, dimensions, regionorigin, regiondimensions;
    int resolution, attenuation, downwardattenuation;
    bool full, scroll;
    int capturerow;
    vector<uchar> solid, sky;
    vector<bvec4> albedo;

    localambientjob(uint serial, const ivec &origin, const ivec &dimensions, const ivec &regionorigin,
                    const ivec &regiondimensions, int resolution, int attenuation, int downwardattenuation, bool full, bool scroll)
        : serial(serial), origin(origin), dimensions(dimensions), regionorigin(regionorigin), regiondimensions(regiondimensions),
          resolution(resolution), attenuation(attenuation), downwardattenuation(downwardattenuation), full(full), scroll(scroll), capturerow(0)
    {
        ZoneScopedN("LocalAmbient/Allocate capture");
        const int cells = regiondimensions.x * regiondimensions.y * regiondimensions.z;
        solid.pad(cells);
        sky.pad(regiondimensions.x * regiondimensions.y);
        albedo.pad(cells);
        // Capture writes every cell before submission; do not touch the whole
        // allocation on the render thread before the budgeted row capture.
    }

    int index(int x, int y, int z) const
    {
        return (z * regiondimensions.y + y) * regiondimensions.x + x;
    }
};

static localambientjob *localambientcapturejob = NULL;

struct localambientregion
{
    ivec origin, dimensions;

    localambientregion() : origin(0, 0, 0), dimensions(0, 0, 0) {}
    localambientregion(const ivec &origin, const ivec &dimensions) : origin(origin), dimensions(dimensions) {}
};

static localambientregion localambientscrollregions[LOCALAMBIENT_SCROLL_REGIONS];
static int localambientscrollregioncount = 0, localambientscrollregionindex = 0;
static vector<localambientjob *> localambientpendingcaptures;
static ivec localambientfieldorigin(0, 0, 0), localambientfielddimensions(0, 0, 0);
static int localambientfieldresolution = 0;
static bool localambientfieldready = false, localambientbootstrap = false;
static GLuint localambientwhitetexture = 0;
static GLuint localambienttexture = 0;

struct localambientsolve
{
    uint serial;
    ivec origin, dimensions, shift;
    int resolution;
    ambientfield::field field;
    vector<localambientjob *> captures;
    std::vector<uchar> solidscratch, skyscratch;
    std::vector<ambientfield::color> albedoscratch;
    SDL_Thread *thread;
    SDL_atomic_t done, cancelled;
    double milliseconds;

    localambientsolve(uint serial, const ivec &origin, const ivec &dimensions, int resolution)
        : serial(serial), origin(origin), dimensions(dimensions), shift(0, 0, 0), resolution(resolution),
          field(dimensions.x, dimensions.y, dimensions.z), thread(NULL), milliseconds(0)
    {
        SDL_AtomicSet(&done, 0);
        SDL_AtomicSet(&cancelled, 0);
    }

    ~localambientsolve()
    {
        captures.deletecontents();
    }
};

// Only the worker touches a submitted snapshot. Retain its storage after
// publication so the next solve neither allocates nor copies a volume here.
static localambientsolve *localambientworker = NULL, *localambientcachedsolve = NULL;
static double localambientlastsolvems = 0;

static uint localambientserial = 1;
static bool localambientdirty = true, localambientdirtyfull = true, localambientdirtyboundsvalid = false;
static bool localambientgirebuild = false;
static ivec localambientdirtymin(0, 0, 0), localambientdirtymax(0, 0, 0);
static int localambientdirtyregions = 1;
static bool localambientdesiredvalid = false;
static ivec localambientdesiredorigin(0, 0, 0), localambientdesireddimensions(0, 0, 0);
static int localambientdesiredresolution = 0, localambientdesiredskylimit = 0;
static int localambientmaxtexturesize = 0;

static bool sameivec(const ivec &a, const ivec &b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

static void clearlocalambientscrollregions()
{
    localambientscrollregioncount = localambientscrollregionindex = 0;
}

static bool haslocalambientscrollregions()
{
    return localambientscrollregionindex < localambientscrollregioncount;
}

static void addlocalambientscrollregion(const ivec &origin, const ivec &dimensions)
{
    if(dimensions.x <= 0 || dimensions.y <= 0 || dimensions.z <= 0 ||
       localambientscrollregioncount >= LOCALAMBIENT_SCROLL_REGIONS) return;
    localambientscrollregions[localambientscrollregioncount++] = localambientregion(origin, dimensions);
}

static void finishlocalambientscrollregion()
{
    if(localambientscrollregionindex < localambientscrollregioncount) localambientscrollregionindex++;
    if(localambientscrollregionindex >= localambientscrollregioncount) clearlocalambientscrollregions();
}

static bool localambientupdatebusy()
{
    return localambientcapturejob || localambientworker || haslocalambientscrollregions();
}

static uint nextlocalambientserial()
{
    if(++localambientserial == 0) ++localambientserial;
    return localambientserial;
}

static int localambientindex(const ivec &dimensions, int x, int y, int z)
{
    return (z * dimensions.y + y) * dimensions.x + x;
}

static void discardlocalambientcapture()
{
    delete localambientcapturejob;
    localambientcapturejob = NULL;
}

static void marklocalambientfull()
{
    nextlocalambientserial();
    if(localambientworker) SDL_AtomicSet(&localambientworker->cancelled, 1);
    discardlocalambientcapture();
    localambientpendingcaptures.deletecontents();
    clearlocalambientscrollregions();
    localambientdirty = localambientdirtyfull = true;
    localambientdirtyboundsvalid = false;
    localambientdirtyregions = 1;
    TracyPlot("LocalAmbient/Dirty regions", int64_t(localambientdirtyregions));
}

void invalidatelocalambient()
{
    if(!localambient) return;
    resetlocalambientfar();
    marklocalambientfull();
}

void invalidatelocalambient(const ivec &minimum, const ivec &maximum)
{
    if(!localambient) return;
    if(minimum.x >= maximum.x || minimum.y >= maximum.y || minimum.z >= maximum.z) return;
    invalidatelocalambientfar(minimum, maximum);
    // Finish the current capture and queue changes for the next one. Its dirty
    // bounds were consumed when it started, so cancelling it loses that work
    // (and continuous chunk publication can prevent any capture from finishing).
    localambientdirty = true;
    if(localambientdirtyregions < INT_MAX) localambientdirtyregions++;
    if(localambientdirtyfull) return;
    if(!localambientdirtyboundsvalid)
    {
        localambientdirtymin = minimum;
        localambientdirtymax = maximum;
        localambientdirtyboundsvalid = true;
    }
    else
    {
        localambientdirtymin.min(minimum);
        localambientdirtymax.max(maximum);
    }
    TracyPlot("LocalAmbient/Dirty regions", int64_t(localambientdirtyregions));
}

void resetlocalambient()
{
    resetlocalambientfar();
    marklocalambientfull();
    delete localambientcachedsolve;
    localambientcachedsolve = NULL;
    localambientfieldready = false;
    localambientbootstrap = false;
    localambientdesiredvalid = false;
}

static void localambienttogglechanged()
{
    cleardeferredlightshaders();
    if(localambient) marklocalambientfull();
    else resetlocalambient();
}

static void localambientfieldchanged()
{
    marklocalambientfull();
}

static void localambientgichanged()
{
    localambientgirebuild = true;
}

bool uselocalambient()
{
    return localambient != 0;
}

bool localambientdebugging()
{
    return localambient && localambientdebug && !drawtex;
}

static void calclocalambientfield(ivec &origin, ivec &dimensions, int &resolution, int &skylimit)
{
    if(!localambientmaxtexturesize)
    {
        glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &localambientmaxtexturesize);
        localambientmaxtexturesize = max(localambientmaxtexturesize, 1);
    }
    resolution = ambientfield::resolution(localambientresolution, localambientmaxdist,
                                         min(int(LOCALAMBIENT_MAX_DIMENSION), localambientmaxtexturesize));
    skylimit = getworldsectionsize() ? min(worldsize, int(WORLD_MAP_SIZE)) : worldsize;
    const int requested = max((2 * localambientmaxdist + resolution - 1) / resolution, 2),
              maximumxy = max(worldsize / resolution, 1), maximumz = max(skylimit / resolution, 1),
              side = min(min(requested, int(LOCALAMBIENT_MAX_DIMENSION)), localambientmaxtexturesize);
    dimensions = ivec(min(side, maximumxy), min(side, maximumxy), min(side, maximumz));
    const int maxstep = max(min(min(dimensions.x, dimensions.y), dimensions.z) / 2, 1),
              stepcells = clamp(localambientscrollstep, 1, maxstep), snap = stepcells * resolution;

    const bool compatible = localambientdesiredvalid && sameivec(dimensions, localambientdesireddimensions) &&
                            resolution == localambientdesiredresolution && skylimit == localambientdesiredskylimit;
    if(compatible)
    {
        origin = localambientdesiredorigin;
        if(!localambientfieldready) return;

        const float deadzone = clamp(localambientdeadzone, 0.1f, 0.9f);
        loopi(3)
        {
            const int limit = i == 2 ? skylimit : worldsize, span = dimensions[i] * resolution,
                      deadspan = clamp(int(floor(span * deadzone)), resolution, span), margin = (span - deadspan) / 2;
            const float position = camera1->o[i], lower = float(origin[i] + margin), upper = float(origin[i] + span - margin);
            int shift = 0;
            if(position < lower)
            {
                const int steps = max(int(ceil((lower - position) / snap)), 1);
                shift = -steps * snap;
            }
            else if(position > upper)
            {
                const int steps = max(int(ceil((position - upper) / snap)), 1);
                shift = steps * snap;
            }
            if(shift)
            {
                origin[i] = clamp(origin[i] + shift, 0, max(limit - span, 0));
                origin[i] = (origin[i] / resolution) * resolution;
            }
        }
        return;
    }

    const int centerx = (int(floor(camera1->o.x / snap)) * snap) + snap / 2,
              centery = (int(floor(camera1->o.y / snap)) * snap) + snap / 2,
              centerz = (int(floor(camera1->o.z / snap)) * snap) + snap / 2;
    origin = ivec(centerx - dimensions.x * resolution / 2, centery - dimensions.y * resolution / 2,
                  centerz - dimensions.z * resolution / 2);
    origin.x = clamp((origin.x / resolution) * resolution, 0, max(worldsize - dimensions.x * resolution, 0));
    origin.y = clamp((origin.y / resolution) * resolution, 0, max(worldsize - dimensions.y * resolution, 0));
    origin.z = clamp((origin.z / resolution) * resolution, 0, max(skylimit - dimensions.z * resolution, 0));
}

static bool capturelocalambient(localambientjob &job)
{
    ZoneScopedN("LocalAmbient/Occupancy capture");
    const Uint64 start = SDL_GetPerformanceCounter();
    const double budget = localambientcapturems * SDL_GetPerformanceFrequency() / 1000.0;
    int captured = 0;
    const int rows = job.regiondimensions.y * job.regiondimensions.z;
    while(job.capturerow < rows && captured + job.regiondimensions.x <= localambientcapturecells)
    {
        const int y = job.capturerow % job.regiondimensions.y, z = job.capturerow / job.regiondimensions.y;
        const ivec cellorigin(job.regionorigin.x, job.regionorigin.y + y, job.regionorigin.z + z),
                   worldorigin(job.origin.x + cellorigin.x * job.resolution, job.origin.y + cellorigin.y * job.resolution,
                               job.origin.z + cellorigin.z * job.resolution);
        captureworldlocalambient(worldorigin, ivec(job.regiondimensions.x, 1, 1), job.resolution,
                                 job.solid.getbuf() + job.index(0, y, z), job.albedo.getbuf() + job.index(0, y, z));
        if(!z) loop(x, job.regiondimensions.x)
        {
            const ivec above(worldorigin.x + x * job.resolution + job.resolution / 2, worldorigin.y + job.resolution / 2,
                             job.origin.z + job.dimensions.z * job.resolution);
            bool visible = true;
            if(above.z < localambientdesiredskylimit)
            {
                if(getworldsectionsize())
                {
                    int roof;
                    if(sampleworldcolumnroof(above, roof)) visible = roof < 0;
                }
                else for(int height = above.z; height < worldsize;)
                {
                    ivec leaf;
                    int size;
                    const cube &c = lookupcube(ivec(above.x, above.y, height), -1, leaf, size);
                    int roof;
                    if(localambientleafroof(c, ivec(above.x, above.y, height), leaf, size, roof)) { visible = false; break; }
                    height = leaf.z + size;
                }
            }
            job.sky[y * job.regiondimensions.x + x] = visible ? 255 : 0;
        }
        captured += job.regiondimensions.x;
        job.capturerow++;
        if(SDL_GetPerformanceCounter() - start >= budget) break;
    }
    TracyPlot("LocalAmbient/Captured cells", int64_t(captured));
    return job.capturerow >= rows;
}

static localambientjob *createlocalambientjob()
{
    ivec regionorigin(0, 0, 0), regiondimensions = localambientdesireddimensions;
    bool full = localambientdirtyfull || !localambientfieldready ||
                !sameivec(localambientdesiredorigin, localambientfieldorigin) ||
                !sameivec(localambientdesireddimensions, localambientfielddimensions) ||
                localambientdesiredresolution != localambientfieldresolution;
    const int downwardattenuation = max(int(ceilf(localambientattenuation * (1.0f - 0.75f * localambientverticalbias))), 1);
    if(!full)
    {
        if(!localambientdirtyboundsvalid) return NULL;
        ivec minimum, maximum;
        loopi(3)
        {
            minimum[i] = int(floor(double(localambientdirtymin[i] - localambientdesiredorigin[i]) / localambientdesiredresolution));
            maximum[i] = int(ceil(double(localambientdirtymax[i] - localambientdesiredorigin[i]) / localambientdesiredresolution));
        }
        if(maximum.x <= 0 || maximum.y <= 0 || maximum.z <= 0 || minimum.x >= localambientdesireddimensions.x ||
           minimum.y >= localambientdesireddimensions.y)
            return NULL;
        minimum.z = 0;
        maximum.z = localambientdesireddimensions.z;
        minimum.max(0);
        maximum.min(localambientdesireddimensions);
        regionorigin = minimum;
        regiondimensions = ivec(maximum).sub(minimum);
        const int regioncells = regiondimensions.x * regiondimensions.y * regiondimensions.z,
                  fieldcells = localambientdesireddimensions.x * localambientdesireddimensions.y * localambientdesireddimensions.z;
        if(regioncells * 4 >= fieldcells * 3)
        {
            full = true;
            regionorigin = ivec(0, 0, 0);
            regiondimensions = localambientdesireddimensions;
        }
    }
    return new localambientjob(localambientserial, localambientdesiredorigin, localambientdesireddimensions, regionorigin, regiondimensions,
                               localambientdesiredresolution, localambientattenuation, downwardattenuation, full, false);
}

static localambientjob *createlocalambientscrolljob()
{
    if(!haslocalambientscrollregions()) return NULL;
    const localambientregion &region = localambientscrollregions[localambientscrollregionindex];
    const int downwardattenuation = max(int(ceilf(localambientattenuation * (1.0f - 0.75f * localambientverticalbias))), 1);
    return new localambientjob(localambientserial, localambientdesiredorigin, localambientdesireddimensions, region.origin, region.dimensions,
                               localambientdesiredresolution, localambientattenuation, downwardattenuation, false, true);
}

static void configurelocalambienttexture(GLuint texture)
{
    glBindTexture(GL_TEXTURE_3D, texture);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_BORDER);
    const GLfloat border[4] = { 1, 0, 0, 0 };
    glTexParameterfv(GL_TEXTURE_3D, GL_TEXTURE_BORDER_COLOR, border);
}


static void bootstraplocalambient(const ivec &origin, const ivec &dimensions, int resolution)
{
    localambientfieldorigin = origin;
    localambientfielddimensions = dimensions;
    localambientfieldresolution = resolution;
    localambientfieldready = localambientbootstrap = true;
}

template<class T>
static void shiftlocalambientsnapshot(std::vector<T> &field, std::vector<T> &scratch, const ivec &dimensions, const ivec &shift)
{
    scratch.resize(field.size());
    const int first = max(-shift.x, 0), last = min(dimensions.x - shift.x, dimensions.x), count = last - first;
    loop(z, dimensions.z) loop(y, dimensions.y)
    {
        const int sourcey = clamp(y + shift.y, 0, dimensions.y - 1), sourcez = clamp(z + shift.z, 0, dimensions.z - 1);
        const T *source = field.data() + localambientindex(dimensions, 0, sourcey, sourcez);
        T *destination = scratch.data() + localambientindex(dimensions, 0, y, z);
        memcpy(destination + first, source + first + shift.x, count * sizeof(T));
        loop(x, first) destination[x] = source[0];
        for(int x = last; x < dimensions.x; x++) destination[x] = source[dimensions.x - 1];
    }
    field.swap(scratch);
}

static bool preparelocalambientsolve(localambientsolve &job)
{
    ZoneScopedN("LocalAmbient/Worker snapshot");
    if(SDL_AtomicGet(&job.cancelled)) return false;
    ambientfield::field &field = job.field;
    if(job.shift.x || job.shift.y || job.shift.z)
    {
        ZoneScopedN("LocalAmbient/Worker scroll");
        shiftlocalambientsnapshot(field.solid, job.solidscratch, job.dimensions, job.shift);
        shiftlocalambientsnapshot(field.albedo, job.albedoscratch, job.dimensions, job.shift);
        shiftlocalambientsnapshot(field.sky, job.skyscratch, ivec(job.dimensions.x, job.dimensions.y, 1), ivec(job.shift.x, job.shift.y, 0));
    }
    field.x = job.dimensions.x;
    field.y = job.dimensions.y;
    field.z = job.dimensions.z;
    const int cells = field.x * field.y * field.z;
    field.solid.resize(cells);
    field.albedo.resize(cells);
    field.sky.resize(field.x * field.y);
    loopv(job.captures)
    {
        const localambientjob &capture = *job.captures[i];
        loop(z, capture.regiondimensions.z) loop(y, capture.regiondimensions.y)
        {
            if(SDL_AtomicGet(&job.cancelled)) return false;
            const int destination = localambientindex(job.dimensions, capture.regionorigin.x, capture.regionorigin.y + y,
                                                       capture.regionorigin.z + z),
                      source = capture.index(0, y, z);
            memcpy(field.solid.data() + destination, capture.solid.getbuf() + source, capture.regiondimensions.x);
            loop(x, capture.regiondimensions.x)
            {
                const bvec4 &albedo = capture.albedo[source + x];
                ambientfield::color &color = field.albedo[destination + x];
                color.r = albedo.r;
                color.g = albedo.g;
                color.b = albedo.b;
            }
        }
        loop(y, capture.regiondimensions.y)
            memcpy(field.sky.data() + (capture.regionorigin.y + y) * field.x + capture.regionorigin.x,
                   capture.sky.getbuf() + y * capture.regiondimensions.x, capture.regiondimensions.x);
    }
    return !SDL_AtomicGet(&job.cancelled);
}

static int runlocalambientsolve(void *data)
{
    ZoneScopedN("LocalAmbient/Worker");
    localambientsolve &job = *static_cast<localambientsolve *>(data);
    const Uint64 start = SDL_GetPerformanceCounter();
    if(preparelocalambientsolve(job))
    {
        ZoneScopedN("LocalAmbient/Worker propagation");
        job.field.solve([&job]() { return SDL_AtomicGet(&job.cancelled) != 0; });
    }
    job.captures.deletecontents();
    job.milliseconds = (SDL_GetPerformanceCounter() - start) * 1000.0 / SDL_GetPerformanceFrequency();
    SDL_AtomicSet(&job.done, 1);
    return 0;
}

static void finishlocalambientsolve()
{
    if(!localambientworker || !SDL_AtomicGet(&localambientworker->done)) return;
    localambientsolve *job = localambientworker;
    SDL_WaitThread(job->thread, NULL);
    localambientworker = NULL;
    if(job->serial == localambientserial && !SDL_AtomicGet(&job->cancelled))
    {
        ZoneScopedN("LocalAmbient/Publish");
        // Upload and transform change together. No draw sees new coordinates
        // with old texels, partially captured slabs, or unfinished propagation.
        if(!localambienttexture) glGenTextures(1, &localambienttexture);
        glActiveTexture_(GL_TEXTURE0);
        configurelocalambienttexture(localambienttexture);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if(!sameivec(job->dimensions, localambientfielddimensions) || localambientbootstrap)
            glTexImage3D_(GL_TEXTURE_3D, 0, GL_RGBA8, job->dimensions.x, job->dimensions.y, job->dimensions.z, 0,
                          GL_RGBA, GL_UNSIGNED_BYTE, job->field.light.data());
        else glTexSubImage3D_(GL_TEXTURE_3D, 0, 0, 0, 0, job->dimensions.x, job->dimensions.y, job->dimensions.z,
                             GL_RGBA, GL_UNSIGNED_BYTE, job->field.light.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_3D, 0);
        localambientfieldorigin = job->origin;
        localambientfielddimensions = job->dimensions;
        localambientfieldresolution = job->resolution;
        localambientfieldready = true;
        localambientbootstrap = false;
        localambientlastsolvems = job->milliseconds;
        TracyPlot("LocalAmbient/Worker milliseconds", job->milliseconds);
        delete localambientcachedsolve;
        localambientcachedsolve = job;
        return;
    }
    delete job;
}

static bool queuelocalambientsolve(const ivec &origin, const ivec &dimensions, int resolution, int attenuation, int downwardattenuation)
{
    ZoneScopedN("LocalAmbient/Submit snapshot");
    if(localambientworker) return false;
    const bool full = !localambientpendingcaptures.empty() && localambientpendingcaptures[0]->full;
    if(!full && (!localambientcachedsolve || !sameivec(localambientcachedsolve->dimensions, dimensions) ||
                 localambientcachedsolve->resolution != resolution)) return false;
    localambientsolve *job = localambientcachedsolve;
    if(!job) job = new localambientsolve(localambientserial, origin, dimensions, resolution);
    localambientcachedsolve = NULL;
    job->shift = full ? ivec(0, 0, 0) : ivec(origin).sub(job->origin).div(resolution);
    job->serial = localambientserial;
    job->origin = origin;
    job->dimensions = dimensions;
    job->resolution = resolution;
    SDL_AtomicSet(&job->done, 0);
    SDL_AtomicSet(&job->cancelled, 0);
    job->captures.move(localambientpendingcaptures);
    // Keep attenuation measured in world units as larger ranges coarsen the grid.
    // Copy settings before starting the thread; the worker only reads its job.
    job->field.loss = clamp((attenuation * resolution + localambientresolution / 2) / localambientresolution, 1, 255);
    job->field.downloss = clamp((downwardattenuation * resolution + localambientresolution / 2) / localambientresolution, 1, 255);
    job->field.gipasses = localambientgi ? (localambientgipasses * localambientresolution + resolution - 1) / resolution : -1;
    job->field.decay = powf(localambientgidecay, float(resolution) / localambientresolution);
    job->thread = SDL_CreateThread(runlocalambientsolve, "local ambient", job);
    if(!job->thread)
    {
        conoutf(CON_ERROR, "local ambient worker failed: %s", SDL_GetError());
        delete job;
        return false;
    }
    localambientworker = job;
    localambientgirebuild = false;
    return true;
}

static void submitlocalambientcapture(localambientjob *job)
{
    // Transfer completed slabs without copying their cells. The worker applies
    // all slabs to its snapshot together, preserving the old rendered origin.
    const ivec origin = job->origin, dimensions = job->dimensions;
    const int resolution = job->resolution, attenuation = job->attenuation, downwardattenuation = job->downwardattenuation;
    const bool scroll = job->scroll;
    localambientpendingcaptures.add(job);
    if(scroll)
    {
        finishlocalambientscrollregion();
        if(haslocalambientscrollregions()) return;
    }
    if(!queuelocalambientsolve(origin, dimensions, resolution, attenuation, downwardattenuation)) marklocalambientfull();
}

static void buildlocalambientscrollregions(const ivec &shift, const ivec &dimensions)
{
    clearlocalambientscrollregions();
    const int x0 = max(-shift.x, 0), x1 = min(dimensions.x - shift.x, dimensions.x),
              y0 = max(-shift.y, 0), y1 = min(dimensions.y - shift.y, dimensions.y),
              z0 = max(-shift.z, 0), z1 = min(dimensions.z - shift.z, dimensions.z);

    if(shift.x > 0) addlocalambientscrollregion(ivec(x1, 0, 0), ivec(dimensions.x - x1, dimensions.y, dimensions.z));
    else if(shift.x < 0) addlocalambientscrollregion(ivec(0, 0, 0), ivec(x0, dimensions.y, dimensions.z));

    if(shift.y > 0) addlocalambientscrollregion(ivec(x0, y1, 0), ivec(x1 - x0, dimensions.y - y1, dimensions.z));
    else if(shift.y < 0) addlocalambientscrollregion(ivec(x0, 0, 0), ivec(x1 - x0, y0, dimensions.z));

    if(shift.z > 0) addlocalambientscrollregion(ivec(x0, y0, z1), ivec(x1 - x0, y1 - y0, dimensions.z - z1));
    else if(shift.z < 0) addlocalambientscrollregion(ivec(x0, y0, 0), ivec(x1 - x0, y1 - y0, z0));
}

static bool scrolllocalambientfield(const ivec &origin)
{
    if(!localambientscroll || !localambientfieldready || localambientbootstrap ||
       !sameivec(localambientfielddimensions, localambientdesireddimensions) ||
       localambientfieldresolution != localambientdesiredresolution) return false;

    const ivec delta(origin.x - localambientfieldorigin.x, origin.y - localambientfieldorigin.y, origin.z - localambientfieldorigin.z);
    if(delta.x % localambientfieldresolution || delta.y % localambientfieldresolution || delta.z % localambientfieldresolution) return false;
    const ivec shift(delta.x / localambientfieldresolution, delta.y / localambientfieldresolution, delta.z / localambientfieldresolution);
    if(!shift.x && !shift.y && !shift.z) return true;
    if(abs(shift.x) >= localambientfielddimensions.x || abs(shift.y) >= localambientfielddimensions.y ||
       abs(shift.z) >= localambientfielddimensions.z) return false;

    ZoneScopedN("LocalAmbient/Scroll");
    nextlocalambientserial();
    buildlocalambientscrollregions(shift, localambientfielddimensions);
    // Keep rendering the previous complete volume while the exposed slabs are
    // captured over subsequent frames. submitlocalambientcapture publishes them together.

    int refreshcells = 0;
    loopi(localambientscrollregioncount)
        refreshcells += localambientscrollregions[i].dimensions.x * localambientscrollregions[i].dimensions.y *
                        localambientscrollregions[i].dimensions.z;
    const int cells = localambientfielddimensions.x * localambientfielddimensions.y * localambientfielddimensions.z;
    (void)cells;
    TracyPlot("LocalAmbient/Scroll reused cells", int64_t(cells - refreshcells));
    TracyPlot("LocalAmbient/Scroll refresh cells", int64_t(refreshcells));
    TracyPlot("LocalAmbient/Scroll regions", int64_t(localambientscrollregioncount));
    return haslocalambientscrollregions();
}

void updatelocalambient()
{
    if(!localambient || !camera1 || !worldroot || drawtex) return;
    updatelocalambientfar();
    ZoneScopedN("LocalAmbient/Update");
    finishlocalambientsolve();

    ivec origin, dimensions;
    int resolution, skylimit;
    calclocalambientfield(origin, dimensions, resolution, skylimit);

    const bool layoutchanged = !localambientdesiredvalid || !sameivec(dimensions, localambientdesireddimensions) ||
                               resolution != localambientdesiredresolution || skylimit != localambientdesiredskylimit;
    if(layoutchanged)
    {
        localambientdesiredorigin = origin;
        localambientdesireddimensions = dimensions;
        localambientdesiredresolution = resolution;
        localambientdesiredskylimit = skylimit;
        localambientdesiredvalid = true;
        marklocalambientfull();
        bootstraplocalambient(origin, dimensions, resolution);
    }
    else if(!sameivec(origin, localambientdesiredorigin))
    {
        if(localambientdirtyfull)
        {
            if(!localambientcapturejob) localambientdesiredorigin = origin;
        }
        else if(localambientfieldready && !localambientupdatebusy())
        {
            localambientdesiredorigin = origin;
            if(!scrolllocalambientfield(origin)) marklocalambientfull();
        }
    }

    if(localambientworker) return;

    if(localambientgirebuild && localambientfieldready && !localambientupdatebusy() && !localambientdirty)
    {
        if(queuelocalambientsolve(localambientfieldorigin, localambientfielddimensions, localambientfieldresolution,
                                localambientattenuation,
                                max(int(ceilf(localambientattenuation * (1.0f - 0.75f * localambientverticalbias))), 1)))
            localambientgirebuild = false;
        else marklocalambientfull();
    }

    if(localambientworker) return;

    if(!localambientcapturejob && haslocalambientscrollregions())
        localambientcapturejob = createlocalambientscrolljob();
    else if(!localambientcapturejob && !haslocalambientscrollregions() && localambientdirty)
    {
        localambientcapturejob = createlocalambientjob();
        localambientdirty = localambientdirtyfull = localambientdirtyboundsvalid = false;
        localambientdirtyregions = 0;
        TracyPlot("LocalAmbient/Dirty regions", int64_t(0));
    }

    if(localambientcapturejob && capturelocalambient(*localambientcapturejob))
    {
        localambientjob *job = localambientcapturejob;
        localambientcapturejob = NULL;
        if(job->serial == localambientserial) submitlocalambientcapture(job);
        else delete job;
    }
}

void bindlocalambient()
{
    if(!localambientwhitetexture)
    {
        const bvec4 white(255, 0, 0, 0);
        glGenTextures(1, &localambientwhitetexture);
        create3dtexture(localambientwhitetexture, 1, 1, 1, &white, 7, 4, GL_RGBA8);
        configurelocalambienttexture(localambientwhitetexture);
    }
    glActiveTexture_(GL_TEXTURE9);
    GLuint texture = localambientwhitetexture;
    if(localambientfieldready && !localambientbootstrap)
        texture = localambienttexture;
    glBindTexture(GL_TEXTURE_3D, texture);
    glActiveTexture_(GL_TEXTURE15);
    glBindTexture(GL_TEXTURE_2D, localambientfartexture);
    glActiveTexture_(GL_TEXTURE0);
}

void setlocalambientparams(bool enabled)
{
    const bool active = enabled && localambientfieldready;
    const int debug = enabled ? localambientdebug : 0;
    const vec origin = active ? vec(localambientfieldorigin) : vec(0, 0, 0);
    const vec scale = active ? vec(1.0f / (localambientfielddimensions.x * localambientfieldresolution),
                                   1.0f / (localambientfielddimensions.y * localambientfieldresolution),
                                   1.0f / (localambientfielddimensions.z * localambientfieldresolution))
                             : vec(0, 0, 0);
    GLOBALPARAM(localambientorigin, origin);
    GLOBALPARAM(localambientscale, scale);
    GLOBALPARAMF(localambientfarparams, localambientfarside ? 1.0f / (localambientfarside * localambientfarspacing) : 0.0f,
                 float(localambientfarspacing), enabled && localambientfar && localambientfarside ? 1.0f : 0.0f, 0.0f);
    GLOBALPARAMF(localambientparams, active ? localambientstrength : 0.0f, localambientmin, float(debug), 2.0f * ldrscale);
    GLOBALPARAMF(localambientgiparams, active && localambientgi && !localambientbootstrap ? 1.0f : 0.0f, localambientgiintensity,
                 localambientgisaturation,
                 localambientgimax);
}

static void localambientstats()
{
    const int cells = localambientfielddimensions.x * localambientfielddimensions.y * localambientfielddimensions.z;
    conoutf(CON_DEBUG, "local ambient: %s, field %dx%dx%d, cell %d, span %dx%dx%d, GPU %d bytes, solve %.2f ms, worker %d, capture %d",
            localambientfieldready && !localambientbootstrap ? "resident" : "daylight fallback",
            localambientfielddimensions.x, localambientfielddimensions.y, localambientfielddimensions.z, localambientfieldresolution,
            localambientfielddimensions.x * localambientfieldresolution, localambientfielddimensions.y * localambientfieldresolution,
            localambientfielddimensions.z * localambientfieldresolution, localambienttexture ? cells * 4 : 0, localambientlastsolvems,
            localambientworker ? 1 : 0, localambientcapturejob ? 1 : 0);
}

COMMAND(localambientstats, "");

void cleanuplocalambient()
{
    resetlocalambient();
    if(localambientworker)
    {
        SDL_AtomicSet(&localambientworker->cancelled, 1);
        SDL_WaitThread(localambientworker->thread, NULL);
        delete localambientworker;
        localambientworker = NULL;
    }
    if(localambientwhitetexture) glDeleteTextures(1, &localambientwhitetexture);
    if(localambienttexture) glDeleteTextures(1, &localambienttexture);
    if(localambientfartexture) glDeleteTextures(1, &localambientfartexture);
    localambientfartexture = 0;
    localambientwhitetexture = localambienttexture = 0;
    localambientgirebuild = false;
    localambientmaxtexturesize = 0;
}
