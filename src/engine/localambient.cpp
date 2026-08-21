// Cheap camera-local daylight accessibility field.

#include "engine.h"
#include "worldruntime.h"

static void localambienttogglechanged();
static void localambientfieldchanged();

VARFP(localambient, 0, 0, 1, localambienttogglechanged());
VARFP(localambientresolution, 4, 16, 128, localambientfieldchanged());
VARFP(localambientmaxdist, 64, 512, 4096, localambientfieldchanged());
VARFP(localambientattenuation, 1, 16, 255, localambientfieldchanged());
FVARFP(localambientverticalbias, 0, 0.25f, 1, localambientfieldchanged());
FVARP(localambientstrength, 0, 1, 1);
FVARP(localambientmin, 0, 0.04f, 1);
VARP(localambientgpupasses, 0, 0, 64);
VARP(localambientcapturecells, 4096, 131072, 524288);
VARP(localambientscroll, 0, 1, 1);
VARP(localambientscrollstep, 1, 4, 32);
FVARP(localambientdeadzone, 0.1f, 0.5f, 0.9f);
VARP(localambientdebug, 0, 0, 1);

enum
{
    LOCALAMBIENT_MAX_DIMENSION = 128,
    LOCALAMBIENT_SCROLL_REGIONS = 3,
    LOCALAMBIENT_GPU_GROUP_SIZE = 4
};


// Compute shaders are core in OpenGL 4.3. Tesseract's legacy GL loader does not expose
// these entry points, so keep the dependency local to this feature.
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif
#ifndef GL_READ_ONLY
#define GL_READ_ONLY 0x88B8
#endif
#ifndef GL_WRITE_ONLY
#define GL_WRITE_ONLY 0x88B9
#endif
#ifndef GL_SHADER_IMAGE_ACCESS_BARRIER_BIT
#define GL_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x00000020
#endif
#ifndef GL_TEXTURE_FETCH_BARRIER_BIT
#define GL_TEXTURE_FETCH_BARRIER_BIT 0x00000008
#endif

typedef void (APIENTRY *localambientdispatchcomputeproc)(GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z);
typedef void (APIENTRY *localambientbindimagetextureproc)(GLuint unit, GLuint texture, GLint level, GLboolean layered,
                                                           GLint layer, GLenum access, GLenum format);
typedef void (APIENTRY *localambientmemorybarrierproc)(GLbitfield barriers);

struct localambientjob
{
    uint serial;
    ivec origin, dimensions, regionorigin, regiondimensions;
    int resolution, attenuation, downwardattenuation;
    bool full, scroll;
    int capturerow;
    vector<uchar> solid;

    localambientjob(uint serial, const ivec &origin, const ivec &dimensions, const ivec &regionorigin,
                    const ivec &regiondimensions, int resolution, int attenuation, int downwardattenuation, bool full, bool scroll)
        : serial(serial), origin(origin), dimensions(dimensions), regionorigin(regionorigin), regiondimensions(regiondimensions),
          resolution(resolution), attenuation(attenuation), downwardattenuation(downwardattenuation), full(full), scroll(scroll), capturerow(0)
    {
        const int cells = regiondimensions.x * regiondimensions.y * regiondimensions.z;
        solid.pad(cells);
        memset(solid.getbuf(), 0, cells);
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
static ivec localambientfieldorigin(0, 0, 0), localambientfielddimensions(0, 0, 0);
static int localambientfieldresolution = 0;
static bool localambientfieldready = false, localambientbootstrap = false;
static GLuint localambientwhitetexture = 0;
static GLuint localambientoccupancytexture = 0, localambientgputextures[2] = { 0, 0 }, localambientgpuprogram = 0,
              localambientgpuseedprogram = 0, localambientgpushiftprogram = 0;
static ivec localambientgputexturedimensions(0, 0, 0);
static int localambientgpufinaltexture = 0;
static bool localambientgpuinitialized = false, localambientgpuavailable = false, localambientgpuready = false;
static bool localambientgpuwarning = false;
static localambientdispatchcomputeproc localambientDispatchCompute = NULL;
static localambientbindimagetextureproc localambientBindImageTexture = NULL;
static localambientmemorybarrierproc localambientMemoryBarrier = NULL;

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
    return localambientcapturejob || haslocalambientscrollregions();
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
    localambientsolidfield.setsize(0);
    localambientscrollscratch.setsize(0);
    localambientgpuready = false;
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
    int captured = 0;
    const int rows = job.regiondimensions.y * job.regiondimensions.z;
    while(job.capturerow < rows && captured + job.regiondimensions.x <= localambientcapturecells)
    {
        const int y = job.capturerow % job.regiondimensions.y, z = job.capturerow / job.regiondimensions.y;
        const ivec cellorigin(job.regionorigin.x, job.regionorigin.y + y, job.regionorigin.z + z),
                   worldorigin(job.origin.x + cellorigin.x * job.resolution, job.origin.y + cellorigin.y * job.resolution,
                               job.origin.z + cellorigin.z * job.resolution);
        captureworldsolid(worldorigin, ivec(job.regiondimensions.x, 1, 1), job.resolution,
                          job.solid.getbuf() + job.index(0, y, z));
        captured += job.regiondimensions.x;
        job.capturerow++;
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
           minimum.y >= localambientdesireddimensions.y || minimum.z >= localambientdesireddimensions.z)
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
    const GLfloat border[4] = { 1, 1, 1, 1 };
    glTexParameterfv(GL_TEXTURE_3D, GL_TEXTURE_BORDER_COLOR, border);
}


static GLint localambientgpudimensionsuniform = -1, localambientgpuattenuationuniform = -1,
             localambientgpudownwardattenuationuniform = -1, localambientgpuseeddimensionsuniform = -1,
             localambientgpushiftdimensionsuniform = -1, localambientgpushiftoffsetuniform = -1;

static const char *localambientcomputesource =
    "#version 430 core\n"
    "layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;\n"
    "layout(r8, binding = 0) readonly uniform image3D occupancyImage;\n"
    "layout(r8, binding = 1) readonly uniform image3D sourceImage;\n"
    "layout(r8, binding = 2) writeonly uniform image3D destinationImage;\n"
    "uniform ivec3 fieldSize;\n"
    "uniform float attenuation;\n"
    "uniform float downwardAttenuation;\n"
    "float lightat(ivec3 p)\n"
    "{\n"
    "    if(any(lessThan(p, ivec3(0))) || any(greaterThanEqual(p, fieldSize))) return 0.0;\n"
    "    return imageLoad(sourceImage, p).r;\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    ivec3 p = ivec3(gl_GlobalInvocationID.xyz);\n"
    "    if(any(greaterThanEqual(p, fieldSize))) return;\n"
    "    if(imageLoad(occupancyImage, p).r > 0.0)\n"
    "    {\n"
    "        imageStore(destinationImage, p, vec4(0.0, 0.0, 0.0, 1.0));\n"
    "        return;\n"
    "    }\n"
    "    float value = imageLoad(sourceImage, p).r;\n"
    "    value = max(value, lightat(p + ivec3(-1, 0, 0)) - attenuation);\n"
    "    value = max(value, lightat(p + ivec3( 1, 0, 0)) - attenuation);\n"
    "    value = max(value, lightat(p + ivec3(0, -1, 0)) - attenuation);\n"
    "    value = max(value, lightat(p + ivec3(0,  1, 0)) - attenuation);\n"
    "    value = max(value, lightat(p + ivec3(0, 0, -1)) - attenuation);\n"
    "    value = max(value, lightat(p + ivec3(0, 0,  1)) - downwardAttenuation);\n"
    "    imageStore(destinationImage, p, vec4(max(value, 0.0), 0.0, 0.0, 1.0));\n"
    "}\n";

static const char *localambientgpuseedsource =
    "#version 430 core\n"
    "layout(local_size_x = 4, local_size_y = 4, local_size_z = 1) in;\n"
    "layout(r8, binding = 0) readonly uniform image3D occupancyImage;\n"
    "layout(r8, binding = 1) writeonly uniform image3D seedImage;\n"
    "uniform ivec3 fieldSize;\n"
    "void main()\n"
    "{\n"
    "    ivec2 column = ivec2(gl_GlobalInvocationID.xy);\n"
    "    if(any(greaterThanEqual(column, fieldSize.xy))) return;\n"
    "    bool skyVisible = true;\n"
    "    for(int z = fieldSize.z - 1; z >= 0; --z)\n"
    "    {\n"
    "        ivec3 p = ivec3(column, z);\n"
    "        bool solid = imageLoad(occupancyImage, p).r > 0.0;\n"
    "        imageStore(seedImage, p, vec4(skyVisible && !solid ? 1.0 : 0.0, 0.0, 0.0, 1.0));\n"
    "        if(solid) skyVisible = false;\n"
    "    }\n"
    "}\n";

static const char *localambientgpushiftsource =
    "#version 430 core\n"
    "layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;\n"
    "layout(r8, binding = 0) readonly uniform image3D sourceImage;\n"
    "layout(r8, binding = 1) writeonly uniform image3D destinationImage;\n"
    "uniform ivec3 fieldSize;\n"
    "uniform ivec3 shiftOffset;\n"
    "void main()\n"
    "{\n"
    "    ivec3 p = ivec3(gl_GlobalInvocationID.xyz);\n"
    "    if(any(greaterThanEqual(p, fieldSize))) return;\n"
    "    ivec3 source = clamp(p + shiftOffset, ivec3(0), fieldSize - ivec3(1));\n"
    "    imageStore(destinationImage, p, imageLoad(sourceImage, source));\n"
    "}\n";

static void localambientgpushaderlog(GLuint object, bool program)
{
    GLint length = 0;
    if(program) glGetProgramiv_(object, GL_INFO_LOG_LENGTH, &length);
    else glGetShaderiv_(object, GL_INFO_LOG_LENGTH, &length);
    if(length <= 1) return;
    GLchar *log = new GLchar[length];
    if(program) glGetProgramInfoLog_(object, length, NULL, log);
    else glGetShaderInfoLog_(object, length, NULL, log);
    conoutf(CON_ERROR, "local ambient GPU shader: %s", log);
    delete[] log;
}

static void bootstraplocalambient(const ivec &origin, const ivec &dimensions, int resolution)
{
    ZoneScopedN("LocalAmbient/Bootstrap");
    localambientfieldorigin = origin;
    localambientfielddimensions = dimensions;
    localambientfieldresolution = resolution;
    localambientfieldready = localambientbootstrap = true;
    localambientgpuready = false;
}

static GLuint createlocalambientcomputeprogram(const char *source)
{
    GLuint shader = glCreateShader_(GL_COMPUTE_SHADER);
    if(!shader) return 0;
    glShaderSource_(shader, 1, &source, NULL);
    glCompileShader_(shader);
    GLint compiled = 0;
    glGetShaderiv_(shader, GL_COMPILE_STATUS, &compiled);
    if(!compiled)
    {
        localambientgpushaderlog(shader, false);
        glDeleteShader_(shader);
        return 0;
    }

    GLuint program = glCreateProgram_();
    glAttachShader_(program, shader);
    glLinkProgram_(program);
    glDeleteShader_(shader);
    GLint linked = 0;
    glGetProgramiv_(program, GL_LINK_STATUS, &linked);
    if(linked) return program;
    localambientgpushaderlog(program, true);
    glDeleteProgram_(program);
    return 0;
}

static bool initlocalambientgpu()
{
    if(localambientgpuinitialized) return localambientgpuavailable;
    localambientgpuinitialized = true;
    localambientgpuavailable = false;

    if(!hasTRG) return false;

    localambientDispatchCompute = (localambientdispatchcomputeproc)SDL_GL_GetProcAddress("glDispatchCompute");
    localambientBindImageTexture = (localambientbindimagetextureproc)SDL_GL_GetProcAddress("glBindImageTexture");
    localambientMemoryBarrier = (localambientmemorybarrierproc)SDL_GL_GetProcAddress("glMemoryBarrier");
    if(!localambientDispatchCompute || !localambientBindImageTexture || !localambientMemoryBarrier)
    {
        if(!localambientgpuwarning)
        {
            conoutf(CON_WARN, "local ambient GPU disabled: OpenGL 4.3 compute entry points are unavailable");
            localambientgpuwarning = true;
        }
        return false;
    }

    localambientgpuprogram = createlocalambientcomputeprogram(localambientcomputesource);
    localambientgpuseedprogram = createlocalambientcomputeprogram(localambientgpuseedsource);
    localambientgpushiftprogram = createlocalambientcomputeprogram(localambientgpushiftsource);
    if(!localambientgpuprogram || !localambientgpuseedprogram || !localambientgpushiftprogram)
    {
        if(localambientgpuprogram) glDeleteProgram_(localambientgpuprogram);
        if(localambientgpuseedprogram) glDeleteProgram_(localambientgpuseedprogram);
        if(localambientgpushiftprogram) glDeleteProgram_(localambientgpushiftprogram);
        localambientgpuprogram = localambientgpuseedprogram = localambientgpushiftprogram = 0;
        if(!localambientgpuwarning)
        {
            conoutf(CON_WARN, "local ambient GPU disabled: compute shader compilation failed (OpenGL 4.3 context required)");
            localambientgpuwarning = true;
        }
        return false;
    }

    localambientgpudimensionsuniform = glGetUniformLocation_(localambientgpuprogram, "fieldSize");
    localambientgpuattenuationuniform = glGetUniformLocation_(localambientgpuprogram, "attenuation");
    localambientgpudownwardattenuationuniform = glGetUniformLocation_(localambientgpuprogram, "downwardAttenuation");
    localambientgpuseeddimensionsuniform = glGetUniformLocation_(localambientgpuseedprogram, "fieldSize");
    localambientgpushiftdimensionsuniform = glGetUniformLocation_(localambientgpushiftprogram, "fieldSize");
    localambientgpushiftoffsetuniform = glGetUniformLocation_(localambientgpushiftprogram, "shiftOffset");
    localambientgpuavailable = true;
    conoutf(CON_DEBUG, "local ambient: GPU compute propagation enabled");
    return true;
}

static void configurelocalambientgpuvolume(GLuint texture, bool linear, bool whiteborder)
{
    glBindTexture(GL_TEXTURE_3D, texture);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, whiteborder ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, whiteborder ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, whiteborder ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE);
    if(whiteborder)
    {
        const GLfloat border[4] = { 1, 1, 1, 1 };
        glTexParameterfv(GL_TEXTURE_3D, GL_TEXTURE_BORDER_COLOR, border);
    }
}

static void deletelocalambientgputextures()
{
    if(localambientoccupancytexture) glDeleteTextures(1, &localambientoccupancytexture);
    if(localambientgputextures[0] || localambientgputextures[1]) glDeleteTextures(2, localambientgputextures);
    localambientoccupancytexture = 0;
    localambientgputextures[0] = localambientgputextures[1] = 0;
    localambientgputexturedimensions = ivec(0, 0, 0);
    localambientgpufinaltexture = 0;
    localambientgpuready = false;
}

static bool ensurelocalambientgputextures(const ivec &dimensions)
{
    if(localambientoccupancytexture && localambientgputextures[0] && localambientgputextures[1] &&
       sameivec(dimensions, localambientgputexturedimensions)) return true;

    deletelocalambientgputextures();
    glGenTextures(1, &localambientoccupancytexture);
    glGenTextures(2, localambientgputextures);
    if(!localambientoccupancytexture || !localambientgputextures[0] || !localambientgputextures[1]) return false;

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    configurelocalambientgpuvolume(localambientoccupancytexture, false, false);
    glTexImage3D_(GL_TEXTURE_3D, 0, GL_R8, dimensions.x, dimensions.y, dimensions.z, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    loopi(2)
    {
        configurelocalambientgpuvolume(localambientgputextures[i], true, true);
        glTexImage3D_(GL_TEXTURE_3D, 0, GL_R8, dimensions.x, dimensions.y, dimensions.z, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_3D, 0);
    localambientgputexturedimensions = dimensions;
    return true;
}

static void alloclocalambientcpufields(const ivec &dimensions)
{
    const int cells = dimensions.x * dimensions.y * dimensions.z;
    localambientsolidfield.setsize(0);
    uchar *solid = localambientsolidfield.pad(cells);
    memset(solid, 0, cells);
}

static bool copylocalambientjobfields(const localambientjob &job)
{
    const int cells = job.dimensions.x * job.dimensions.y * job.dimensions.z;
    if(job.full || localambientsolidfield.length() != cells) alloclocalambientcpufields(job.dimensions);
    if(localambientsolidfield.length() != cells) return false;

    loop(z, job.regiondimensions.z) loop(y, job.regiondimensions.y)
    {
        const int destination = localambientindex(job.dimensions, job.regionorigin.x, job.regionorigin.y + y, job.regionorigin.z + z),
                  source = job.index(0, y, z);
        memcpy(localambientsolidfield.getbuf() + destination, job.solid.getbuf() + source, job.regiondimensions.x);
    }
    return true;
}

static bool shiftlocalambientgpuoccupancy(const ivec &dimensions, const ivec &shift)
{
    if(!localambientgpuready || !ensurelocalambientgputextures(dimensions)) return false;
    glUseProgram_(localambientgpushiftprogram);
    if(localambientgpushiftdimensionsuniform >= 0)
        glUniform3i_(localambientgpushiftdimensionsuniform, dimensions.x, dimensions.y, dimensions.z);
    if(localambientgpushiftoffsetuniform >= 0) glUniform3i_(localambientgpushiftoffsetuniform, shift.x, shift.y, shift.z);
    localambientBindImageTexture(0, localambientoccupancytexture, 0, GL_TRUE, 0, GL_READ_ONLY, GL_R8);
    localambientBindImageTexture(1, localambientgputextures[0], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R8);
    localambientDispatchCompute((dimensions.x + LOCALAMBIENT_GPU_GROUP_SIZE - 1) / LOCALAMBIENT_GPU_GROUP_SIZE,
                                (dimensions.y + LOCALAMBIENT_GPU_GROUP_SIZE - 1) / LOCALAMBIENT_GPU_GROUP_SIZE,
                                (dimensions.z + LOCALAMBIENT_GPU_GROUP_SIZE - 1) / LOCALAMBIENT_GPU_GROUP_SIZE);
    localambientMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    swap(localambientoccupancytexture, localambientgputextures[0]);
    configurelocalambientgpuvolume(localambientoccupancytexture, false, false);
    configurelocalambientgpuvolume(localambientgputextures[0], true, true);
    glBindTexture(GL_TEXTURE_3D, 0);
    return true;
}

static bool rebuildlocalambientgpu(const ivec &origin, const ivec &dimensions, int resolution, int attenuation, int downwardattenuation,
                                   const localambientjob *uploadjob = NULL, bool uploadoccupancy = true)
{
    if(!initlocalambientgpu()) return false;
    const int cells = dimensions.x * dimensions.y * dimensions.z;
    if(localambientsolidfield.length() != cells) return false;
    const bool texturesready = localambientoccupancytexture && localambientgputextures[0] && localambientgputextures[1] &&
                               sameivec(dimensions, localambientgputexturedimensions);
    if(!ensurelocalambientgputextures(dimensions)) return false;

    const Uint64 start = SDL_GetPerformanceCounter();
    if(uploadoccupancy)
    {
        ZoneScopedN("LocalAmbient/Occupancy upload");
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glBindTexture(GL_TEXTURE_3D, localambientoccupancytexture);
        if(texturesready && uploadjob && !uploadjob->full)
            glTexSubImage3D_(GL_TEXTURE_3D, 0, uploadjob->regionorigin.x, uploadjob->regionorigin.y, uploadjob->regionorigin.z,
                             uploadjob->regiondimensions.x, uploadjob->regiondimensions.y, uploadjob->regiondimensions.z, GL_RED,
                             GL_UNSIGNED_BYTE, uploadjob->solid.getbuf());
        else glTexSubImage3D_(GL_TEXTURE_3D, 0, 0, 0, 0, dimensions.x, dimensions.y, dimensions.z, GL_RED, GL_UNSIGNED_BYTE,
                              localambientsolidfield.getbuf());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_3D, 0);
    }

    {
        ZoneScopedN("LocalAmbient/GPU seed");
        glUseProgram_(localambientgpuseedprogram);
        if(localambientgpuseeddimensionsuniform >= 0)
            glUniform3i_(localambientgpuseeddimensionsuniform, dimensions.x, dimensions.y, dimensions.z);
        localambientBindImageTexture(0, localambientoccupancytexture, 0, GL_TRUE, 0, GL_READ_ONLY, GL_R8);
        localambientBindImageTexture(1, localambientgputextures[0], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R8);
        localambientDispatchCompute((dimensions.x + LOCALAMBIENT_GPU_GROUP_SIZE - 1) / LOCALAMBIENT_GPU_GROUP_SIZE,
                                    (dimensions.y + LOCALAMBIENT_GPU_GROUP_SIZE - 1) / LOCALAMBIENT_GPU_GROUP_SIZE, 1);
        localambientMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }

    const int minloss = max(min(attenuation, downwardattenuation), 1),
              passes = localambientgpupasses > 0 ? localambientgpupasses : clamp((255 + minloss - 1) / minloss, 1, 64),
              groupsx = (dimensions.x + LOCALAMBIENT_GPU_GROUP_SIZE - 1) / LOCALAMBIENT_GPU_GROUP_SIZE,
              groupsy = (dimensions.y + LOCALAMBIENT_GPU_GROUP_SIZE - 1) / LOCALAMBIENT_GPU_GROUP_SIZE,
              groupsz = (dimensions.z + LOCALAMBIENT_GPU_GROUP_SIZE - 1) / LOCALAMBIENT_GPU_GROUP_SIZE;

    {
        ZoneScopedN("LocalAmbient/GPU propagate");
        glUseProgram_(localambientgpuprogram);
        if(localambientgpudimensionsuniform >= 0) glUniform3i_(localambientgpudimensionsuniform, dimensions.x, dimensions.y, dimensions.z);
        if(localambientgpuattenuationuniform >= 0) glUniform1f_(localambientgpuattenuationuniform, attenuation / 255.0f);
        if(localambientgpudownwardattenuationuniform >= 0)
            glUniform1f_(localambientgpudownwardattenuationuniform, downwardattenuation / 255.0f);

        int source = 0, destination = 1;
        loopi(passes)
        {
            localambientBindImageTexture(0, localambientoccupancytexture, 0, GL_TRUE, 0, GL_READ_ONLY, GL_R8);
            localambientBindImageTexture(1, localambientgputextures[source], 0, GL_TRUE, 0, GL_READ_ONLY, GL_R8);
            localambientBindImageTexture(2, localambientgputextures[destination], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R8);
            localambientDispatchCompute(groupsx, groupsy, groupsz);
            localambientMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            swap(source, destination);
        }
        localambientgpufinaltexture = source;
    }
    localambientMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    localambientBindImageTexture(0, 0, 0, GL_TRUE, 0, GL_READ_ONLY, GL_R8);
    localambientBindImageTexture(1, 0, 0, GL_TRUE, 0, GL_READ_ONLY, GL_R8);
    localambientBindImageTexture(2, 0, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R8);
    glUseProgram_(0);

    localambientgpuready = true;
    localambientfieldorigin = origin;
    localambientfielddimensions = dimensions;
    localambientfieldresolution = resolution;
    localambientfieldready = true;
    localambientbootstrap = false;

    const double milliseconds = (SDL_GetPerformanceCounter() - start) * 1000.0 / SDL_GetPerformanceFrequency();
    (void)milliseconds;
    TracyPlot("LocalAmbient/GPU passes", int64_t(passes));
    TracyPlot("LocalAmbient/GPU cells per rebuild", int64_t(cells));
    TracyPlot("LocalAmbient/GPU dispatch milliseconds", milliseconds);
    return true;
}

static bool uploadlocalambientgpu(localambientjob &job)
{
    if(!copylocalambientjobfields(job)) return false;
    return rebuildlocalambientgpu(job.origin, job.dimensions, job.resolution, job.attenuation, job.downwardattenuation, &job);
}

static bool shiftlocalambientcpufield(vector<uchar> &field, const ivec &dimensions, const ivec &shift)
{
    const int cells = dimensions.x * dimensions.y * dimensions.z;
    if(field.length() != cells) return false;
    localambientscrollscratch.setsize(0);
    uchar *shifted = localambientscrollscratch.pad(cells);
    loop(z, dimensions.z) loop(y, dimensions.y) loop(x, dimensions.x)
    {
        int sourcex = x + shift.x, sourcey = y + shift.y, sourcez = z + shift.z;
        sourcex = clamp(sourcex, 0, dimensions.x - 1);
        sourcey = clamp(sourcey, 0, dimensions.y - 1);
        sourcez = clamp(sourcez, 0, dimensions.z - 1);
        shifted[localambientindex(dimensions, x, y, z)] = field[localambientindex(dimensions, sourcex, sourcey, sourcez)];
    }
    memcpy(field.getbuf(), shifted, cells);
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

static bool scrolllocalambientgpufield(const ivec &origin)
{
    if(!localambientscroll || !localambientfieldready || !localambientgpuready ||
       !sameivec(localambientfielddimensions, localambientdesireddimensions) ||
       localambientfieldresolution != localambientdesiredresolution) return false;

    const ivec delta(origin.x - localambientfieldorigin.x, origin.y - localambientfieldorigin.y, origin.z - localambientfieldorigin.z);
    if(delta.x % localambientfieldresolution || delta.y % localambientfieldresolution || delta.z % localambientfieldresolution) return false;
    const ivec shift(delta.x / localambientfieldresolution, delta.y / localambientfieldresolution, delta.z / localambientfieldresolution);
    if(!shift.x && !shift.y && !shift.z) return true;
    if(abs(shift.x) >= localambientfielddimensions.x || abs(shift.y) >= localambientfielddimensions.y ||
       abs(shift.z) >= localambientfielddimensions.z) return false;

    ZoneScopedN("LocalAmbient/GPU scroll");
    if(!shiftlocalambientcpufield(localambientsolidfield, localambientfielddimensions, shift)) return false;
    if(!shiftlocalambientgpuoccupancy(localambientfielddimensions, shift)) return false;

    nextlocalambientserial();
    buildlocalambientscrollregions(shift, localambientfielddimensions);
    const int downwardattenuation = max(int(ceilf(localambientattenuation * (1.0f - 0.75f * localambientverticalbias))), 1);
    if(!rebuildlocalambientgpu(origin, localambientfielddimensions, localambientfieldresolution, localambientattenuation,
                               downwardattenuation, NULL, false)) return false;

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
    const bool computeavailable = initlocalambientgpu();

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
            if(!scrolllocalambientgpufield(origin)) marklocalambientfull();
        }
    }

    if(!computeavailable) return;

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
            if(uploadlocalambientgpu(*job))
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
        const uchar white = 255;
        glGenTextures(1, &localambientwhitetexture);
        create3dtexture(localambientwhitetexture, 1, 1, 1, &white, 7, 1, hasTRG ? GL_R8 : GL_LUMINANCE8);
        configurelocalambienttexture(localambientwhitetexture);
    }
    glActiveTexture_(GL_TEXTURE9);
    GLuint texture = localambientwhitetexture;
    if(localambientfieldready && !localambientbootstrap && localambientgpuready)
        texture = localambientgputextures[localambientgpufinaltexture];
    glBindTexture(GL_TEXTURE_3D, texture);
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
    const int cells = localambientfielddimensions.x * localambientfielddimensions.y * localambientfielddimensions.z;
    const int resident = localambientgpuavailable ? cells * 4 : 0;
    conoutf(CON_DEBUG, "local ambient: %s, %s, field %dx%dx%d at %d, resident ~%d bytes, capture %d, scroll %d/%d",
            localambientfieldready ? "resident" : "not resident", localambientgpuavailable ? "GPU compute" : "daylight bootstrap",
            localambientfielddimensions.x, localambientfielddimensions.y, localambientfielddimensions.z, localambientfieldresolution, resident,
            localambientcapturejob ? 1 : 0, localambientscrollregionindex, localambientscrollregioncount);
}

COMMAND(localambientstats, "");

void cleanuplocalambient()
{
    discardlocalambientcapture();
    if(localambientwhitetexture) glDeleteTextures(1, &localambientwhitetexture);
    localambientwhitetexture = 0;
    deletelocalambientgputextures();
    if(localambientgpuprogram) glDeleteProgram_(localambientgpuprogram);
    if(localambientgpuseedprogram) glDeleteProgram_(localambientgpuseedprogram);
    if(localambientgpushiftprogram) glDeleteProgram_(localambientgpushiftprogram);
    localambientgpuprogram = localambientgpuseedprogram = localambientgpushiftprogram = 0;
    localambientgpudimensionsuniform = localambientgpuattenuationuniform = localambientgpudownwardattenuationuniform =
        localambientgpuseeddimensionsuniform = localambientgpushiftdimensionsuniform = localambientgpushiftoffsetuniform = -1;
    localambientgpuinitialized = localambientgpuavailable = localambientgpuready = false;
    localambientDispatchCompute = NULL;
    localambientBindImageTexture = NULL;
    localambientMemoryBarrier = NULL;
    localambientsolidfield.setsize(0);
    localambientscrollscratch.setsize(0);
    clearlocalambientscrollregions();
    localambientfieldready = localambientbootstrap = localambientdesiredvalid = false;
    localambientmaxtexturesize = 0;
}
