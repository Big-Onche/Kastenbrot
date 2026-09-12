#include "watermeshbuild.h"
// Persistent section water attachments. Included by material.cpp after corner sampling.
struct watermeshstate
{
    vec4 endpoints; // min(height0 + wave * weight0, height1 + wave * weight1)
    vec2 surface; // spatial wave multiplier, undisplaced surface for pass selection
};

struct waterresource
{
    worldmeshrange vertices, indices, state;
    vector<watermeshpatch> patches;
    int first[8], count[8];
    uint version;
    bool topologydirty;

    waterresource() : version(0), topologydirty(false)
    {
        memset(first, 0, sizeof(first));
        memset(count, 0, sizeof(count));
    }
};

static uint waterstateversion = 1;
static vector<waterresource *> visiblewater;

void releasewaterresource(waterresource *&resource)
{
    if(!resource) return;
    visiblewater.removeobj(resource);
    if(resource->vertices.buffer) destroyvbo(resource->vertices.buffer);
    if(resource->indices.buffer) destroyvbo(resource->indices.buffer);
    if(resource->state.buffer) destroyvbo(resource->state.buffer);
    DELETEP(resource);
}

void releasewaterresource(vtxarray &va)
{
    releasewaterresource(va.water);
}

static bool watermeshcell(int x, int y, int z)
{
    return (lookupmaterial(vec(x, y, z)) & MATF_VOLUME) == MAT_WATER;
}

static void updatewaterstate(waterresource &resource)
{
    if(resource.version == waterstateversion) return;
    ZoneScopedN("Water/Upload simulation state");
    vector<watermeshstate> states;
    loopv(resource.patches)
    {
        const watermeshpatch &patch = resource.patches[i];
        const waterfacepatch &face = patch.face;
        const ivec &o = face.origin;
        vec4 heights[4];
        float spatial = 0, reference = o.z;
        if(face.orient == O_TOP)
        {
            const int x[4] = { o.x, o.x + face.rsize, o.x + face.rsize, o.x },
                      y[4] = { o.y, o.y, o.y + face.csize, o.y + face.csize };
            const int bx = o.x & ~15, by = o.y & ~15;
            const float drops[4] = { getwatercornerdrop(bx, by, o.z), getwatercornerdrop(bx + 16, by, o.z),
                                     getwatercornerdrop(bx + 16, by + 16, o.z), getwatercornerdrop(bx, by + 16, o.z) };
            bool falling = false;
            const int level = getwatercelllevel(ivec(o.x + face.rsize / 2, o.y + face.csize / 2, o.z - 1), falling);
            bool flowing = level >= 0 && !falling;
            loopj(4) if(drops[j] != 0) flowing = true;
            spatial = flowing ? 0 : 1;
            reference -= level > 0 && !falling ? min(level, 7) * 2.0f : 0;
            loopj(4)
            {
                const float u = (x[j] - bx) / 16.0f, v = (y[j] - by) / 16.0f;
                const float drop = (1 - v) * ((1 - u) * drops[0] + u * drops[1]) + v * ((1 - u) * drops[3] + u * drops[2]);
                const float z = o.z - (flowing ? drop : 0);
                heights[j] = vec4(z, 1, z, 1);
            }
        }
        else if(face.orient == O_BOTTOM)
        {
            loopj(4) heights[j] = vec4(o.z - 0.1f, 0, o.z - 0.1f, 0);
        }
        else
        {
            const int dim = dimension(face.orient), along = 1 - dim, sign = dimcoord(face.orient) ? 1 : -1,
                      width = dim == 0 ? face.rsize : face.csize, height = dim == 0 ? face.csize : face.rsize;
            ivec a(o), b(o), inside(o), outside(o);
            b[along] += width;
            inside[along] += width / 2;
            outside[along] += width / 2;
            if(sign > 0) --inside[dim];
            else --outside[dim];
            const int top = o.z + height;
            int surface = top;
            bool upper = false;
            for(; surface <= top + 128; surface += 16)
                if(watermeshcell(inside.x, inside.y, surface - 1) && !watermeshcell(inside.x, inside.y, surface))
                {
                    upper = true;
                    break;
                }
            const bool lower = watermeshcell(outside.x, outside.y, o.z - 1) && !watermeshcell(outside.x, outside.y, o.z);
            const float az = upper ? surface - getwatercornerdrop(a.x, a.y, surface) : float(top),
                        bz = upper ? surface - getwatercornerdrop(b.x, b.y, surface) : float(top),
                        cap = surface != top ? float(top) : 1e16f;
            heights[3] = vec4(az, upper ? 1 : 0, cap, 0);
            heights[2] = vec4(bz, upper ? 1 : 0, cap, 0);
            // Lower endpoints are capped by the upper endpoint in the shader too.
            heights[0] = vec4(lower ? o.z - getwatercornerdrop(a.x, a.y, o.z) : float(o.z), lower ? 1 : 0, az, upper ? 1 : 0);
            heights[1] = vec4(lower ? o.z - getwatercornerdrop(b.x, b.y, o.z) : float(o.z), lower ? 1 : 0, bz, upper ? 1 : 0);
            if((dim == 0) != (sign > 0)) swap(heights[1], heights[3]);
        }
        loopj(4)
        {
            watermeshstate &state = states.add();
            state.endpoints = heights[j];
            state.surface = vec2(spatial, reference - WATER_OFFSET);
        }
    }
    // Updating simulation state publishes a fresh pooled range; in-flight draws keep their old bytes.
    uploadworldmesh(resource.state, GL_ARRAY_BUFFER, states.getbuf(), states.length() * sizeof(watermeshstate));
    resource.version = waterstateversion;
}

void uploadwatermeshpacket(waterresource *&attachment, watermeshpacket &packet)
{
    releasewaterresource(attachment);
    if(packet.indices.empty()) return;
    attachment = new waterresource;
    waterresource &resource = *attachment;
    resource.patches.move(packet.patches);
    memcpy(resource.first, packet.first, sizeof(resource.first));
    memcpy(resource.count, packet.count, sizeof(resource.count));
    uploadworldmesh(resource.vertices, GL_ARRAY_BUFFER, packet.vertices.getbuf(), packet.vertices.length() * sizeof(watermeshvertex));
    uploadworldmesh(resource.indices, GL_ELEMENT_ARRAY_BUFFER, packet.indices.getbuf(), packet.indices.length() * sizeof(uint));
    updatewaterstate(resource);
}

void buildwaterresource(waterresource *&attachment, const materialsurface *surfaces, int count)
{
    watermeshpacket packet;
    buildwatermeshpacket(packet, surfaces, count, lookupwatergeometrycell);
    uploadwatermeshpacket(attachment, packet);
}
void buildwaterresource(vtxarray &va)
{
    buildwaterresource(va.water, va.matbuf, va.matsurfs);
}

static void drawwaterresource(waterresource &resource, int group, int side)
{
    if(!resource.count[group]) return;
    updatewaterstate(resource);
    LOCALPARAMF(watermeshparams, 1.0f, vertwater && drawtex != DRAWTEX_MINIMAP ? 1.0f : 0.0f,
                fmod(float(lastmillis / 600.0f / (2 * M_PI)), 1.0f), float(side));
    gle::disable();
    glBindBuffer_(GL_ARRAY_BUFFER, resource.vertices.buffer);
    gle::vertexpointer(sizeof(watermeshvertex), (void *)(size_t)resource.vertices.offset, GL_FLOAT, 4);
    gle::normalpointer(sizeof(watermeshvertex), (void *)(size_t)(resource.vertices.offset + offsetof(watermeshvertex, normal)));
    gle::enablevertex();
    gle::enablenormal();
    glBindBuffer_(GL_ARRAY_BUFFER, resource.state.buffer);
    gle::texcoord0pointer(sizeof(watermeshstate), (void *)(size_t)resource.state.offset, GL_FLOAT, 4);
    gle::texcoord1pointer(sizeof(watermeshstate), (void *)(size_t)(resource.state.offset + offsetof(watermeshstate, surface)));
    gle::enabletexcoord0();
    gle::enabletexcoord1();
    glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER, resource.indices.buffer);
    glDrawElements(GL_TRIANGLES, resource.count[group], GL_UNSIGNED_INT,
                   (void *)(size_t)(resource.indices.offset + resource.first[group] * sizeof(uint)));
    xtraverts += resource.count[group];
    gle::disablevertex();
    gle::disablenormal();
    gle::disabletexcoord0();
    gle::disabletexcoord1();
    glBindBuffer_(GL_ARRAY_BUFFER, 0);
    glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER, 0);
    LOCALPARAMF(watermeshparams, 0.0f, 0.0f, 0.0f, 0.0f);
}

bool haswatergeometry(int material)
{
    loopv(visiblewater) if(visiblewater[i]->count[material]) return true;
    return false;
}

bool haswaterfallgeometry(int material)
{
    if(drawtex == DRAWTEX_MINIMAP) return false;
    loopv(visiblewater) if(visiblewater[i]->count[material + 4]) return true;
    return false;
}

void renderwatergeometry(int material, bool mask, int side)
{
    loopv(visiblewater) drawwaterresource(*visiblewater[i], material, mask ? 0 : side);
}

void renderwaterfallgeometry(int material, bool mask)
{
    if(drawtex == DRAWTEX_MINIMAP) return;
    loopv(visiblewater) drawwaterresource(*visiblewater[i], material + 4, 0);
}
