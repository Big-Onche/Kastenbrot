// Cheap camera-local daylight accessibility field.

#include "engine.h"
#include "worldruntime.h"

static void localambienttogglechanged();
static void localambientfieldchanged();

VARFP(localambient, 0, 0, 1, localambienttogglechanged());
VARFP(localambientresolution, 1, 16, 128, localambientfieldchanged());
VARFP(localambientmaxdist, 64, 512, 4096, localambientfieldchanged());
VARFP(localambientattenuation, 1, 4, 255, localambientfieldchanged());
FVARFP(localambientverticalbias, 0, 0.25f, 1, localambientfieldchanged());
FVARP(localambientstrength, 0, 1, 1);
FVARP(localambientmin, 0, 0.04f, 1);
VARP(localambientupdates, 1, 1, 8);
VARP(localambientdebug, 0, 0, 1);

enum
{
    LOCALAMBIENT_MAX_DIMENSION = 128,
    LOCALAMBIENT_CAPTURE_CELLS = 32768
};

struct localambientjob
{
    uint serial;
    ivec origin, dimensions, regionorigin, regiondimensions;
    int resolution, attenuation, downwardattenuation, skylimit;
    bool full;
    int capturecolumn, capturez;
    bool captureblocked;
    vector<uchar> solid, seeds, light;
    int cellsprocessed, maxqueuesize;
    double buildmilliseconds;

    localambientjob(uint serial, const ivec &origin, const ivec &dimensions, const ivec &regionorigin,
                    const ivec &regiondimensions, int resolution, int attenuation, int downwardattenuation, int skylimit, bool full)
        : serial(serial), origin(origin), dimensions(dimensions), regionorigin(regionorigin), regiondimensions(regiondimensions),
          resolution(resolution), attenuation(attenuation), downwardattenuation(downwardattenuation), skylimit(skylimit), full(full),
          capturecolumn(0), capturez(regiondimensions.z - 1), captureblocked(false), cellsprocessed(0), maxqueuesize(0),
          buildmilliseconds(0)
    {
        const int cells = regiondimensions.x * regiondimensions.y * regiondimensions.z;

        solid.pad(cells);
        seeds.pad(cells);
        light.pad(cells);

        memset(solid.getbuf(), 0, cells);
        memset(seeds.getbuf(), 0, cells);
        memset(light.getbuf(), 0, cells);
    }

    int index(int x, int y, int z) const
    {
        return (z * regiondimensions.y + y) * regiondimensions.x + x;
    }
};

static SDL_mutex *localambientmutex = NULL;
static SDL_cond *localambientcond = NULL;
static SDL_Thread *localambientthread = NULL;
static localambientjob *localambientpendingjob = NULL, *localambientresultjob = NULL;
static bool stoplocalambientthread = false, localambientworkeractive = false;

static localambientjob *localambientcapturejob = NULL;
static vector<uchar> localambientfield;
static ivec localambientfieldorigin(0, 0, 0), localambientfielddimensions(0, 0, 0);
static int localambientfieldresolution = 0;
static bool localambientfieldready = false;
static GLuint localambienttexture = 0, localambientwhitetexture = 0;

static uint localambientserial = 1;
static bool localambientdirty = true, localambientdirtyfull = true, localambientdirtyboundsvalid = false;
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

static uint nextlocalambientserial()
{
    if(++localambientserial == 0) ++localambientserial;
    return localambientserial;
}

static int localambientindex(const ivec &dimensions, int x, int y, int z)
{
    return (z * dimensions.y + y) * dimensions.x + x;
}

static bool localambientcellsolid(int x, int y, int z)
{
    if(x < 0 || y < 0 || z < 0 || x >= worldsize || y >= worldsize || z >= worldsize) return true;
    int leafbottom;
    return sampleworldsolid(ivec(x, y, z), leafbottom);
}

static bool localambientblockedabove(int x, int y, int z, int skylimit)
{
    for(int samplez = skylimit - 1; samplez >= z;)
    {
        int leafbottom;
        if(sampleworldsolid(ivec(x, y, samplez), leafbottom)) return true;
        int nextz = leafbottom - 1;
        samplez = nextz < samplez ? nextz : samplez - 1;
    }
    return false;
}

static int localambientpropagationloss(const localambientjob &job, int sourcez, int neighborz)
{
    return neighborz < sourcez ? job.downwardattenuation : job.attenuation;
}

static void buildlocalambient(localambientjob &job)
{
    ZoneScopedN("LocalAmbient/Build");
    const Uint64 start = SDL_GetPerformanceCounter();
    const int cells = job.regiondimensions.x * job.regiondimensions.y * job.regiondimensions.z;
    vector<int> queue;
    queue.reserve(cells);

    {
        ZoneScopedN("LocalAmbient/Seed");
        loopi(cells) if(!job.solid[i] && job.seeds[i])
        {
            job.light[i] = job.seeds[i];
            queue.add(i);
        }
    }

    {
        ZoneScopedN("LocalAmbient/Propagate");
        static const int directions[6][3] =
        {
            { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }
        };
        for(int head = 0; head < queue.length(); ++head)
        {
            const int current = queue[head], plane = job.regiondimensions.x * job.regiondimensions.y,
                      z = current / plane, remainder = current - z * plane,
                      y = remainder / job.regiondimensions.x, x = remainder - y * job.regiondimensions.x;
            const int source = job.light[current];
            loopi(6)
            {
                const int nx = x + directions[i][0], ny = y + directions[i][1], nz = z + directions[i][2];
                if(nx < 0 || ny < 0 || nz < 0 || nx >= job.regiondimensions.x || ny >= job.regiondimensions.y ||
                   nz >= job.regiondimensions.z)
                    continue;
                const int neighbor = job.index(nx, ny, nz);
                if(job.solid[neighbor]) continue;
                const int candidate = source - localambientpropagationloss(job, z, nz);
                if(candidate <= job.light[neighbor]) continue;
                job.light[neighbor] = uchar(candidate);
                queue.add(neighbor);
            }
            job.cellsprocessed++;
        }
        job.maxqueuesize = queue.length();
    }

    job.buildmilliseconds = (SDL_GetPerformanceCounter() - start) * 1000.0 / SDL_GetPerformanceFrequency();
    TracyPlot("LocalAmbient/Cells processed", int64_t(job.cellsprocessed));
    TracyPlot("LocalAmbient/Propagation queue", int64_t(job.maxqueuesize));
    TracyPlot("LocalAmbient/Build milliseconds", job.buildmilliseconds);
}

static int localambientworker(void *)
{
    for(;;)
    {
        SDL_LockMutex(localambientmutex);
        while(!localambientpendingjob && !stoplocalambientthread) SDL_CondWait(localambientcond, localambientmutex);
        if(stoplocalambientthread)
        {
            SDL_UnlockMutex(localambientmutex);
            return 0;
        }
        localambientjob *job = localambientpendingjob;
        localambientpendingjob = NULL;
        localambientworkeractive = true;
        SDL_UnlockMutex(localambientmutex);

        buildlocalambient(*job);

        SDL_LockMutex(localambientmutex);
        localambientworkeractive = false;
        while(localambientresultjob && !stoplocalambientthread) SDL_CondWait(localambientcond, localambientmutex);
        if(stoplocalambientthread)
        {
            SDL_UnlockMutex(localambientmutex);
            delete job;
            return 0;
        }
        localambientresultjob = job;
        SDL_CondBroadcast(localambientcond);
        SDL_UnlockMutex(localambientmutex);
    }
}

static bool startlocalambientworker()
{
    if(localambientthread) return true;
    if(!localambientmutex) localambientmutex = SDL_CreateMutex();
    if(!localambientcond) localambientcond = SDL_CreateCond();
    if(!localambientmutex || !localambientcond) return false;
    stoplocalambientthread = false;
    localambientthread = SDL_CreateThread(localambientworker, "local ambient worker", NULL);
    return localambientthread != NULL;
}

static void stoplocalambientworker()
{
    if(localambientmutex)
    {
        SDL_LockMutex(localambientmutex);
        stoplocalambientthread = true;
        SDL_CondBroadcast(localambientcond);
        SDL_UnlockMutex(localambientmutex);
    }
    if(localambientthread)
    {
        SDL_WaitThread(localambientthread, NULL);
        localambientthread = NULL;
    }
    delete localambientpendingjob;
    delete localambientresultjob;
    localambientpendingjob = localambientresultjob = NULL;
    localambientworkeractive = false;
    if(localambientcond) SDL_DestroyCond(localambientcond);
    if(localambientmutex) SDL_DestroyMutex(localambientmutex);
    localambientcond = NULL;
    localambientmutex = NULL;
    stoplocalambientthread = false;
}

static void discardlocalambientcapture()
{
    delete localambientcapturejob;
    localambientcapturejob = NULL;
}

static void marklocalambientfull()
{
    nextlocalambientserial();
    discardlocalambientcapture();
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
    nextlocalambientserial();
    discardlocalambientcapture();
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
    localambientfield.setsize(0);
    localambientfieldready = false;
    localambientdesiredvalid = false;
    if(localambientmutex)
    {
        SDL_LockMutex(localambientmutex);
        delete localambientpendingjob;
        delete localambientresultjob;
        localambientpendingjob = localambientresultjob = NULL;
        SDL_CondBroadcast(localambientcond);
        SDL_UnlockMutex(localambientmutex);
    }
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
    resolution = max(localambientresolution, 1);
    skylimit = getworldsectionsize() ? min(worldsize, int(WORLD_MAP_SIZE)) : worldsize;
    const int requested = max((2 * localambientmaxdist + resolution - 1) / resolution, 2),
              maximumxy = max(worldsize / resolution, 1), maximumz = max(skylimit / resolution, 1),
              side = min(min(requested, int(LOCALAMBIENT_MAX_DIMENSION)), localambientmaxtexturesize);
    dimensions = ivec(min(side, maximumxy), min(side, maximumxy), min(side, maximumz));
    const int snap = max(getworldsectionsize() ? getworldsectionsize() : int(WORLD_SECTION_SIZE), resolution),
              centerx = (int(floor(camera1->o.x / snap)) * snap) + snap / 2,
              centery = (int(floor(camera1->o.y / snap)) * snap) + snap / 2,
              centerz = (int(floor(camera1->o.z / snap)) * snap) + snap / 2;
    origin = ivec(centerx - dimensions.x * resolution / 2, centery - dimensions.y * resolution / 2,
                  centerz - dimensions.z * resolution / 2);
    origin.x = clamp((origin.x / resolution) * resolution, 0, max(worldsize - dimensions.x * resolution, 0));
    origin.y = clamp((origin.y / resolution) * resolution, 0, max(worldsize - dimensions.y * resolution, 0));
    origin.z = clamp((origin.z / resolution) * resolution, 0, max(skylimit - dimensions.z * resolution, 0));
}

static void addlocalambientboundaryseeds(localambientjob &job)
{
    if(job.full || !localambientfieldready || !sameivec(job.origin, localambientfieldorigin) ||
       !sameivec(job.dimensions, localambientfielddimensions) || job.resolution != localambientfieldresolution)
        return;
    static const int directions[6][3] =
    {
        { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }
    };
    loop(z, job.regiondimensions.z) loop(y, job.regiondimensions.y) loop(x, job.regiondimensions.x)
    {
        if(x && x + 1 < job.regiondimensions.x && y && y + 1 < job.regiondimensions.y && z && z + 1 < job.regiondimensions.z) continue;
        const int fieldx = job.regionorigin.x + x, fieldy = job.regionorigin.y + y, fieldz = job.regionorigin.z + z,
                  target = job.index(x, y, z);
        loopi(6)
        {
            const int nx = fieldx + directions[i][0], ny = fieldy + directions[i][1], nz = fieldz + directions[i][2];
            if(nx < 0 || ny < 0 || nz < 0 || nx >= job.dimensions.x || ny >= job.dimensions.y || nz >= job.dimensions.z ||
               (nx >= job.regionorigin.x && nx < job.regionorigin.x + job.regiondimensions.x &&
                ny >= job.regionorigin.y && ny < job.regionorigin.y + job.regiondimensions.y &&
                nz >= job.regionorigin.z && nz < job.regionorigin.z + job.regiondimensions.z))
                continue;
            const int source = localambientfield[localambientindex(job.dimensions, nx, ny, nz)],
                      loss = localambientpropagationloss(job, nz, fieldz), candidate = source - loss;
            if(candidate > job.seeds[target]) job.seeds[target] = uchar(candidate);
        }
    }
}

static bool capturelocalambient(localambientjob &job)
{
    ZoneScopedN("LocalAmbient/Seed");
    int captured = 0;
    const int columns = job.regiondimensions.x * job.regiondimensions.y;
    while(job.capturecolumn < columns && captured < LOCALAMBIENT_CAPTURE_CELLS)
    {
        const int x = job.capturecolumn % job.regiondimensions.x, y = job.capturecolumn / job.regiondimensions.x,
                  fieldx = job.regionorigin.x + x, fieldy = job.regionorigin.y + y,
                  worldx = job.origin.x + fieldx * job.resolution + job.resolution / 2,
                  worldy = job.origin.y + fieldy * job.resolution + job.resolution / 2;
        if(job.capturez == job.regiondimensions.z - 1)
        {
            const int fieldz = job.regionorigin.z + job.capturez,
                      worldtop = job.origin.z + (fieldz + 1) * job.resolution;
            job.captureblocked = localambientblockedabove(worldx, worldy, worldtop, job.skylimit);
        }
        while(job.capturez >= 0 && captured < LOCALAMBIENT_CAPTURE_CELLS)
        {
            const int fieldz = job.regionorigin.z + job.capturez,
                      worldz = job.origin.z + fieldz * job.resolution + job.resolution / 2,
                      index = job.index(x, y, job.capturez);
            const bool solid = localambientcellsolid(worldx, worldy, worldz);
            job.solid[index] = solid ? 1 : 0;
            if(!job.captureblocked && !solid) job.seeds[index] = 255;
            if(solid) job.captureblocked = true;
            job.capturez--;
            captured++;
        }
        if(job.capturez < 0)
        {
            job.capturecolumn++;
            job.capturez = job.regiondimensions.z - 1;
        }
    }
    TracyPlot("LocalAmbient/Captured cells", int64_t(captured));
    if(job.capturecolumn < columns) return false;
    addlocalambientboundaryseeds(job);
    return true;
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
           minimum.y >= localambientdesireddimensions.y || minimum.z >= localambientdesireddimensions.z)
            return NULL;
        const int propagation = (255 + downwardattenuation - 1) / downwardattenuation;
        minimum.x -= propagation;
        minimum.y -= propagation;
        maximum.x += propagation;
        maximum.y += propagation;
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
                               localambientdesiredresolution, localambientattenuation, downwardattenuation,
                               localambientdesiredskylimit, full);
}

static void queuelocalambientjob(localambientjob *job)
{
    if(!startlocalambientworker())
    {
        buildlocalambient(*job);
        delete localambientresultjob;
        localambientresultjob = job;
        return;
    }
    SDL_LockMutex(localambientmutex);
    delete localambientpendingjob;
    localambientpendingjob = job;
    TracyPlot("LocalAmbient/Pending builds", int64_t(1));
    SDL_CondSignal(localambientcond);
    SDL_UnlockMutex(localambientmutex);
}

static void configurelocalambienttexture(GLuint texture)
{
    glBindTexture(GL_TEXTURE_3D, texture);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_BORDER);
    const GLfloat border[4] = { 1, 1, 1, 1 };
    glTexParameterfv(GL_TEXTURE_3D, GL_TEXTURE_BORDER_COLOR, border);
}

static void uploadlocalambient(localambientjob &job)
{
    ZoneScopedN("LocalAmbient/Upload");
    const GLenum component = hasTRG ? GL_R8 : GL_LUMINANCE8, format = hasTRG ? GL_RED : GL_LUMINANCE;
    const int bytes = job.regiondimensions.x * job.regiondimensions.y * job.regiondimensions.z;
    (void)bytes;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if(job.full || !localambienttexture || !localambientfieldready)
    {
        if(!localambienttexture) glGenTextures(1, &localambienttexture);
        create3dtexture(localambienttexture, job.dimensions.x, job.dimensions.y, job.dimensions.z, job.light.getbuf(), 7, 1, component);
        configurelocalambienttexture(localambienttexture);
        localambientfield = job.light;
        localambientfieldorigin = job.origin;
        localambientfielddimensions = job.dimensions;
        localambientfieldresolution = job.resolution;
        localambientfieldready = true;
    }
    else
    {
        configurelocalambienttexture(localambienttexture);
        glTexSubImage3D_(GL_TEXTURE_3D, 0, job.regionorigin.x, job.regionorigin.y, job.regionorigin.z, job.regiondimensions.x,
                         job.regiondimensions.y, job.regiondimensions.z, format, GL_UNSIGNED_BYTE, job.light.getbuf());
        loop(z, job.regiondimensions.z) loop(y, job.regiondimensions.y)
        {
            const int destination = localambientindex(job.dimensions, job.regionorigin.x, job.regionorigin.y + y, job.regionorigin.z + z),
                      source = job.index(0, y, z);
            memcpy(localambientfield.getbuf() + destination, job.light.getbuf() + source, job.regiondimensions.x);
        }
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_3D, 0);
    TracyPlot("LocalAmbient/Upload bytes", int64_t(bytes));
    TracyPlot("LocalAmbient/Resident bytes", int64_t(localambientfield.length() * 2));
}

static int processlocalambientresults()
{
    int uploads = 0;
    for(;;)
    {
        localambientjob *job = NULL;
        if(localambientmutex)
        {
            SDL_LockMutex(localambientmutex);
            job = localambientresultjob;
            localambientresultjob = NULL;
            if(job) SDL_CondBroadcast(localambientcond);
            SDL_UnlockMutex(localambientmutex);
        }
        else
        {
            job = localambientresultjob;
            localambientresultjob = NULL;
        }
        if(!job) break;
        if(job->serial == localambientserial && uploads < localambientupdates)
        {
            uploadlocalambient(*job);
            uploads++;
        }
        delete job;
        if(uploads >= localambientupdates) break;
    }
    return uploads;
}

void updatelocalambient()
{
    if(!localambient || !camera1 || !worldroot || drawtex) return;
    processlocalambientresults();

    ivec origin, dimensions;
    int resolution, skylimit;
    calclocalambientfield(origin, dimensions, resolution, skylimit);
    if(!localambientdesiredvalid || !sameivec(origin, localambientdesiredorigin) || !sameivec(dimensions, localambientdesireddimensions) ||
       resolution != localambientdesiredresolution || skylimit != localambientdesiredskylimit)
    {
        localambientdesiredorigin = origin;
        localambientdesireddimensions = dimensions;
        localambientdesiredresolution = resolution;
        localambientdesiredskylimit = skylimit;
        localambientdesiredvalid = true;
        marklocalambientfull();
    }

    if(!localambientcapturejob && localambientdirty)
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
        queuelocalambientjob(job);
    }
}

void bindlocalambient()
{
    if(!localambientwhitetexture)
    {
        const uchar white = 255;
        glGenTextures(1, &localambientwhitetexture);
        create3dtexture(localambientwhitetexture, 1, 1, 1, &white, 7, 1, hasTRG ? GL_R8 : GL_LUMINANCE8);
        configurelocalambienttexture(localambientwhitetexture);
    }
    glActiveTexture_(GL_TEXTURE9);
    glBindTexture(GL_TEXTURE_3D, localambientfieldready ? localambienttexture : localambientwhitetexture);
    glActiveTexture_(GL_TEXTURE0);
}

void setlocalambientparams(bool enabled)
{
    const bool active = enabled && localambientfieldready, debug = enabled && localambientdebug;
    const vec origin = active ? vec(localambientfieldorigin) : vec(0, 0, 0);
    const vec scale = active ? vec(1.0f / (localambientfielddimensions.x * localambientfieldresolution),
                                   1.0f / (localambientfielddimensions.y * localambientfieldresolution),
                                   1.0f / (localambientfielddimensions.z * localambientfieldresolution))
                             : vec(0, 0, 0);
    GLOBALPARAM(localambientorigin, origin);
    GLOBALPARAM(localambientscale, scale);
    GLOBALPARAMF(localambientparams, active ? localambientstrength : 0.0f, localambientmin, debug ? 1.0f : 0.0f, 2.0f * ldrscale);
}

static void localambientstats()
{
    int pending = 0, active = 0, ready = 0;
    if(localambientmutex)
    {
        SDL_LockMutex(localambientmutex);
        pending = localambientpendingjob ? 1 : 0;
        active = localambientworkeractive ? 1 : 0;
        ready = localambientresultjob ? 1 : 0;
        SDL_UnlockMutex(localambientmutex);
    }
    conoutf(CON_DEBUG, "local ambient: %s, field %dx%dx%d at %d, CPU/GPU resident %d bytes, capture %d, pending %d, active %d, ready %d",
            localambientfieldready ? "resident" : "not resident", localambientfielddimensions.x, localambientfielddimensions.y,
            localambientfielddimensions.z, localambientfieldresolution, localambientfield.length() * 2,
            localambientcapturejob ? 1 : 0, pending, active, ready);
}

COMMAND(localambientstats, "");

void cleanuplocalambient()
{
    discardlocalambientcapture();
    stoplocalambientworker();
    if(localambienttexture) glDeleteTextures(1, &localambienttexture);
    if(localambientwhitetexture) glDeleteTextures(1, &localambientwhitetexture);
    localambienttexture = localambientwhitetexture = 0;
    localambientfield.setsize(0);
    localambientfieldready = localambientdesiredvalid = false;
    localambientmaxtexturesize = 0;
}
