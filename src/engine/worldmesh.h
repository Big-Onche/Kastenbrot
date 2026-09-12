#ifndef ENGINE_WORLDMESH_H
#define ENGINE_WORLDMESH_H

#include "watermeshpacket.h"

// Disposable render data. None of these objects owns gameplay cubes or VAs.
// VSlot identity includes shader, textures, uniforms, scroll, alpha and refraction
// settings. Orientation and environment selection remain explicit. Clip/volume
// material bits are gameplay metadata and do not change the surface draw state.
struct worldmeshbatchkey
{
    ushort texture, envmap;
    uchar orient, layer;
    bool alpha;

    worldmeshbatchkey() : texture(0), envmap(EMID_NONE), orient(0), layer(LAYER_TOP), alpha(false)
    {
    }

    bool operator==(const worldmeshbatchkey &b) const
    {
        return texture == b.texture && envmap == b.envmap && orient == b.orient && layer == b.layer && alpha == b.alpha;
    }

    bool operator<(const worldmeshbatchkey &b) const
    {
        if(alpha != b.alpha) return alpha < b.alpha;
        if(texture != b.texture) return texture < b.texture;
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
    vector<worldmeshdrawrange> ranges;
    vector<materialsurface> materials;
    ullong revision, published;
    ullong request;
    waterresource *water;
    bool dirty, pending, edited;
    int sourceranges;

    worldmeshsection(const ivec &origin)
        : origin(origin), minimum(origin), maximum(origin), revision(1), published(0), request(0), water(NULL),
          dirty(true), pending(false), edited(false), sourceranges(0)
    {
    }
};

extern void clearworldmeshpackets();
extern int worldmeshpackets;
extern void dirtyworldmeshpackets(const ivec &minimum, const ivec &maximum, bool edited = false);
extern void discardworldmeshsection(const ivec &origin);
extern int processworldmeshpackets(double budget, int uploadlimit);
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
