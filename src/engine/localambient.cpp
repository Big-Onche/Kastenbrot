// Camera-local daylight and color bounce, solved on immutable worker snapshots.

#include "localambientfield.h"
#include "engine.h"
#include "worldruntime.h"

static void localambienttogglechanged();
static void localambientfieldchanged();
static void localambientgichanged();

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
        const int cells = regiondimensions.x * regiondimensions.y * regiondimensions.z;
        solid.pad(cells);
        sky.pad(regiondimensions.x * regiondimensions.y);
        albedo.pad(cells);
        memset(solid.getbuf(), 0, cells);
        loopi(cells) albedo[i] = bvec4(0, 0, 0, 0);
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
static vector<uchar> localambientscrollscratch;
static vector<uchar> localambientsolidfield;
static vector<bvec4> localambientalbedofield, localambientalbedoscrollscratch;
static ivec localambientfieldorigin(0, 0, 0), localambientfielddimensions(0, 0, 0);
static int localambientfieldresolution = 0;
static bool localambientfieldready = false, localambientbootstrap = false;
static GLuint localambientwhitetexture = 0;
static GLuint localambienttexture = 0;
static vector<uchar> localambientskyfield, localambientskyscratch;

struct localambientsolve
{
    uint serial;
    ivec origin, dimensions;
    int resolution;
    ambientfield::field field;
    SDL_Thread *thread;
    SDL_atomic_t done, cancelled;
    double milliseconds;

    localambientsolve(uint serial, const ivec &origin, const ivec &dimensions, int resolution)
        : serial(serial), origin(origin), dimensions(dimensions), resolution(resolution),
          field(dimensions.x, dimensions.y, dimensions.z), thread(NULL), milliseconds(0)
    {
        SDL_AtomicSet(&done, 0);
        SDL_AtomicSet(&cancelled, 0);
    }
};

static localambientsolve *localambientworker = NULL;
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
    clearlocalambientscrollregions();
    localambientdirty = localambientdirtyfull = true;
    localambientdirtyboundsvalid = false;
    localambientdirtyregions = 1;
    TracyPlot("LocalAmbient/Dirty regions", int64_t(localambientdirtyregions));
}

void invalidatelocalambient()
{
    if(!localambient) return;
    marklocalambientfull();
}

void invalidatelocalambient(const ivec &minimum, const ivec &maximum)
{
    if(!localambient) return;
    if(minimum.x >= maximum.x || minimum.y >= maximum.y || minimum.z >= maximum.z) return;
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
    marklocalambientfull();
    localambientsolidfield.setsize(0);
    localambientalbedofield.setsize(0);
    localambientscrollscratch.setsize(0);
    localambientalbedoscrollscratch.setsize(0);
    localambientskyfield.setsize(0);
    localambientskyscratch.setsize(0);
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
                    if(!isempty(c)) { visible = false; break; }
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

static int runlocalambientsolve(void *data)
{
    localambientsolve &job = *static_cast<localambientsolve *>(data);
    const Uint64 start = SDL_GetPerformanceCounter();
    job.field.solve([&job]() { return SDL_AtomicGet(&job.cancelled) != 0; });
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
    }
    delete job;
}

static void alloclocalambientcpufields(const ivec &dimensions)
{
    const int cells = dimensions.x * dimensions.y * dimensions.z;
    localambientsolidfield.setsize(0);
    localambientalbedofield.setsize(0);
    localambientskyfield.setsize(0);
    memset(localambientskyfield.pad(dimensions.x * dimensions.y), 255, dimensions.x * dimensions.y);
    uchar *solid = localambientsolidfield.pad(cells);
    localambientalbedofield.pad(cells);
    memset(solid, 0, cells);
    loopi(cells) localambientalbedofield[i] = bvec4(0, 0, 0, 0);
}

static bool copylocalambientjobfields(const localambientjob &job)
{
    const int cells = job.dimensions.x * job.dimensions.y * job.dimensions.z;
    if(job.full || localambientsolidfield.length() != cells || localambientalbedofield.length() != cells)
        alloclocalambientcpufields(job.dimensions);
    if(localambientsolidfield.length() != cells || localambientalbedofield.length() != cells) return false;

    loop(z, job.regiondimensions.z) loop(y, job.regiondimensions.y)
    {
        const int destination = localambientindex(job.dimensions, job.regionorigin.x, job.regionorigin.y + y, job.regionorigin.z + z),
                  source = job.index(0, y, z);
        memcpy(localambientsolidfield.getbuf() + destination, job.solid.getbuf() + source, job.regiondimensions.x);
        memcpy(localambientalbedofield.getbuf() + destination, job.albedo.getbuf() + source, job.regiondimensions.x * sizeof(bvec4));
    }
    loop(y, job.regiondimensions.y)
        memcpy(localambientskyfield.getbuf() + (job.regionorigin.y + y) * job.dimensions.x + job.regionorigin.x,
               job.sky.getbuf() + y * job.regiondimensions.x, job.regiondimensions.x);
    return true;
}

static bool queuelocalambientsolve(const ivec &origin, const ivec &dimensions, int resolution, int attenuation, int downwardattenuation)
{
    const int cells = dimensions.x * dimensions.y * dimensions.z;
    if(localambientworker || localambientsolidfield.length() != cells || localambientalbedofield.length() != cells) return false;
    localambientsolve *job = new localambientsolve(localambientserial, origin, dimensions, resolution);
    job->field.solid.assign(localambientsolidfield.getbuf(), localambientsolidfield.getbuf() + cells);
    job->field.sky.assign(localambientskyfield.getbuf(), localambientskyfield.getbuf() + dimensions.x * dimensions.y);
    job->field.albedo.resize(cells);
    loopi(cells)
    {
        ambientfield::color &color = job->field.albedo[i];
        color.r = localambientalbedofield[i].r;
        color.g = localambientalbedofield[i].g;
        color.b = localambientalbedofield[i].b;
    }
    // Keep attenuation measured in world units as larger ranges coarsen the grid.
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

static bool submitlocalambientcapture(localambientjob &job)
{
    if(!copylocalambientjobfields(job)) return false;
    if(job.scroll)
    {
        // The CPU field has moved, but the rendered volume still uses its old
        // origin. Publish the entire shifted field only after every slab is real.
        if(localambientscrollregionindex + 1 < localambientscrollregioncount) return true;
        return queuelocalambientsolve(job.origin, job.dimensions, job.resolution, job.attenuation, job.downwardattenuation);
    }
    return queuelocalambientsolve(job.origin, job.dimensions, job.resolution, job.attenuation, job.downwardattenuation);
}

template<class T>
static bool shiftlocalambientcpufield(vector<T> &field, vector<T> &scratch, const ivec &dimensions, const ivec &shift)
{
    const int cells = dimensions.x * dimensions.y * dimensions.z;
    if(field.length() != cells) return false;
    scratch.setsize(0);
    T *shifted = scratch.pad(cells);
    const int first = max(-shift.x, 0), last = min(dimensions.x - shift.x, dimensions.x), count = last - first;
    // Copy the overlapping part of each row in bulk, extending the boundary cells
    // into newly exposed columns exactly as the previous per-cell clamp did.
    loop(z, dimensions.z) loop(y, dimensions.y)
    {
        const int sourcey = clamp(y + shift.y, 0, dimensions.y - 1), sourcez = clamp(z + shift.z, 0, dimensions.z - 1);
        const T *source = field.getbuf() + localambientindex(dimensions, 0, sourcey, sourcez);
        T *destination = shifted + localambientindex(dimensions, 0, y, z);
        memcpy(destination + first, source + first + shift.x, count * sizeof(T));
        loop(x, first) destination[x] = source[0];
        for(int x = last; x < dimensions.x; x++) destination[x] = source[dimensions.x - 1];
    }
    // vector::move swaps storage when the destination is empty, retaining both
    // allocations for the next scroll without copying the entire volume back.
    field.setsize(0);
    field.move(scratch);
    return true;
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
    {
        ZoneScopedN("LocalAmbient/CPU scroll");
        if(!shiftlocalambientcpufield(localambientsolidfield, localambientscrollscratch, localambientfielddimensions, shift)) return false;
        if(!shiftlocalambientcpufield(localambientalbedofield, localambientalbedoscrollscratch, localambientfielddimensions, shift)) return false;
        if(!shiftlocalambientcpufield(localambientskyfield, localambientskyscratch,
                                     ivec(localambientfielddimensions.x, localambientfielddimensions.y, 1), ivec(shift.x, shift.y, 0))) return false;
    }

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
        if(job->serial == localambientserial)
        {
            if(submitlocalambientcapture(*job))
            {
                if(job->scroll) finishlocalambientscrollregion();
            }
            else marklocalambientfull();
        }
        delete job;
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
    localambientwhitetexture = localambienttexture = 0;
    localambientgirebuild = false;
    localambientmaxtexturesize = 0;
}
