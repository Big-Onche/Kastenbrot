#ifndef ENGINE_WORLDMESH_H
#define ENGINE_WORLDMESH_H

#include "watermeshpacket.h"

// Disposable render data. None of these objects owns gameplay cubes or VAs.
// Loose materials retain VSlot identity. Array materials replace that identity
// with an array and canonical uniform state; layer, climate mode and MSAA source
// identity live in the vertices. Orientation and environment remain explicit.
enum { WORLDMESH_OPAQUE, WORLDMESH_CUTOUT, WORLDMESH_TRANSLUCENT, WORLDMESH_REFRACTIVE };

struct Texture;
struct VSlot;
enum { BLOCKARRAY_OPAQUE, BLOCKARRAY_CUTOUT, BLOCKARRAY_SCATTER };
extern void beginblocktexturearrays();
extern void registerblocktexture(Texture *texture, int renderclass);
extern void bakeblocktexturearrays();
extern void cleanupblocktexturearrays();
extern void updateblocktexturearrayfilter();
extern uint getblocktexturearraygeneration();
extern GLuint lookupblocktexturearray(Texture *texture, int renderclass, ushort &layer);
extern int blocktexturemode(const VSlot &slot);
extern bool compatibleblocktexturestate(const VSlot &a, const VSlot &b);

struct worldmeshbatchkey
{
    ushort texture, envmap;
    uchar orient, layer;
    bool alpha;
    uchar renderclass;
    bool twosided;
    GLuint texturearray = 0;
    ushort arraystate = 0;

    worldmeshbatchkey() : texture(0), envmap(EMID_NONE), orient(0), layer(LAYER_TOP), alpha(false), renderclass(WORLDMESH_OPAQUE), twosided(false)
    {
    }

    bool operator==(const worldmeshbatchkey &b) const
    {
        return texturearray == b.texturearray && (texturearray ? arraystate == b.arraystate : texture == b.texture) &&
               envmap == b.envmap && orient == b.orient && layer == b.layer &&
               renderclass == b.renderclass && twosided == b.twosided;
    }

    bool operator<(const worldmeshbatchkey &b) const
    {
        if(renderclass != b.renderclass) return renderclass < b.renderclass;
        if(twosided != b.twosided) return twosided < b.twosided;
        if(texturearray != b.texturearray) return texturearray < b.texturearray;
        if(texturearray) { if(arraystate != b.arraystate) return arraystate < b.arraystate; }
        else if(texture != b.texture) return texture < b.texture;
        if(envmap != b.envmap) return envmap < b.envmap;
        if(layer != b.layer) return layer < b.layer;
        return orient < b.orient;
    }
};

struct worldmeshdrawrange : worldmeshbatchkey
{
    uint first, count;
    ushort material;

    worldmeshdrawrange() : first(0), count(0), material(0)
    {
    }
};

struct worldmeshpacket
{
    vector<vertex> vertices;
    vector<uint> indices;
    vector<worldmeshdrawrange> ranges;
    vector<materialsurface> materials;
    watermeshpacket water;
    ivec minimum, maximum;
    int sourceranges;

    worldmeshpacket() : minimum(0, 0, 0), maximum(0, 0, 0), sourceranges(0)
    {
    }
};

struct worldmeshsection
{
    ivec origin, minimum, maximum;
    worldmeshrange vertices, indices;
    // Published CPU data allows budgeted GPU compaction without rebuilding the
    // octree mesh. Indices remain section-relative; GPU uploads rebase a copy.
    vector<vertex> rendervertices;
    vector<uint> renderindices;
    vector<worldmeshdrawrange> ranges;
    vector<materialsurface> materials;
    ullong revision, published;
    ullong request;
    waterresource *water;
    bool dirty, pending, edited;
    int sourceranges, alphapasses;

    worldmeshsection(const ivec &origin)
        : origin(origin), minimum(origin), maximum(origin), revision(1), published(0), request(0), water(NULL),
          dirty(true), pending(false), edited(false), sourceranges(0), alphapasses(0)
    {
    }
};

extern void clearworldmeshpackets();
extern int worldmeshpackets;
extern void dirtyworldmeshpackets(const ivec &minimum, const ivec &maximum, bool edited = false);
extern void discardworldmeshsection(const ivec &origin);
extern int processworldmeshpackets(double budget, int uploadlimit, bool editsonly = false);
extern bool worldmeshpacketpending(const ivec &origin);
extern const vector<worldmeshsection *> &getworldmeshsections();
extern ullong getworldmeshgeneration();
extern void beginworldmeshdrawstats();
extern void endworldmeshdrawstats();
extern void invalidateworldmeshcsm();
extern void queueworldmeshworld();
extern bool worldmeshsectionvisible(const worldmeshsection &section);
extern void renderworldmeshgeometry(int side = 0, bool shadow = false, bool rsm = false, bool refractmask = false);
extern void buildwaterresource(waterresource *&resource, const materialsurface *surfaces, int count);
extern void releasewaterresource(waterresource *&resource);
extern void uploadwatermeshpacket(waterresource *&resource, watermeshpacket &packet);
extern void setupworldmeshmaterials(materialsurface *surfaces, int count);

#endif
