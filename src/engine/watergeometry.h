#ifndef __WATER_GEOMETRY_H__
#define __WATER_GEOMETRY_H__

struct waterfacepatch
{
    ivec origin;
    int orient, rsize, csize;

    waterfacepatch() : origin(0, 0, 0), orient(O_TOP), rsize(0), csize(0) {}
    waterfacepatch(const ivec &origin, int orient, int rsize, int csize)
        : origin(origin), orient(orient), rsize(rsize), csize(csize) {}
};

struct watergeometrycell
{
    ivec origin;
    int size;
    bool water, occludes;

    watergeometrycell(const ivec &origin, int size, bool water, bool occludes)
        : origin(origin), size(size), water(water), occludes(occludes) {}
};

static inline bool waterfallpatchless(const waterfacepatch &a, const waterfacepatch &b)
{
    if(a.orient != b.orient) return a.orient < b.orient;
    if(a.origin.x != b.origin.x) return a.origin.x < b.origin.x;
    if(a.origin.y != b.origin.y) return a.origin.y < b.origin.y;
    const int awidth = dimension(a.orient) == 0 ? a.rsize : a.csize, bwidth = dimension(b.orient) == 0 ? b.rsize : b.csize;
    if(awidth != bwidth) return awidth < bwidth;
    return a.origin.z < b.origin.z;
}

// Cell boundaries determine visibility, not tessellation. Rejoin exposed vertical runs before fitting their endpoints.
static inline void mergewaterfallpatches(vector<waterfacepatch> &patches)
{
    patches.sort(waterfallpatchless);
    int count = 0;
    loopv(patches)
    {
        const waterfacepatch patch = patches[i];
        if(count && dimension(patch.orient) < 2)
        {
            waterfacepatch &previous = patches[count - 1];
            const int dim = dimension(patch.orient);
            int &height = dim == 0 ? previous.csize : previous.rsize;
            if(previous.orient == patch.orient && previous.origin.x == patch.origin.x && previous.origin.y == patch.origin.y &&
               (dim == 0 ? previous.rsize == patch.rsize : previous.csize == patch.csize) && previous.origin.z + height == patch.origin.z)
            {
                height += dim == 0 ? patch.csize : patch.rsize;
                continue;
            }
        }
        patches[count++] = patch;
    }
    patches.setsize(count);
}

// Resolve only water/air boundaries. Leaf extents skip whole submerged rectangles, including mismatched octree sizes.
// Partially exposed merged faces are split at actual cell boundaries before any corner fitting or vertex generation.
template<class CellAt, class Emit>
static void exposedwaterpatches(const waterfacepatch &face, CellAt cellat, Emit emit, vector<waterfacepatch> &pending)
{
    const int dim = dimension(face.orient), row = R[dim], col = C[dim];
    pending.setsize(0);
    pending.add(face);
    while(!pending.empty())
    {
        waterfacepatch patch = pending.pop();
        ivec inside(patch.origin), outside(patch.origin);
        if(dimcoord(face.orient)) --inside[dim];
        else --outside[dim];
        const watergeometrycell source = cellat(inside), neighbour = cellat(outside);
        const int rsize = min(patch.rsize, min(source.origin[row] + source.size, neighbour.origin[row] + neighbour.size) - patch.origin[row]),
                  csize = min(patch.csize, min(source.origin[col] + source.size, neighbour.origin[col] + neighbour.size) - patch.origin[col]);
        if(rsize <= 0 || csize <= 0) continue;
        if(rsize < patch.rsize)
        {
            ivec origin(patch.origin);
            origin[row] += rsize;
            pending.add(waterfacepatch(origin, face.orient, patch.rsize - rsize, patch.csize));
        }
        if(csize < patch.csize)
        {
            ivec origin(patch.origin);
            origin[col] += csize;
            pending.add(waterfacepatch(origin, face.orient, rsize, patch.csize - csize));
        }
        if(source.water && !neighbour.water && !neighbour.occludes)
            emit(waterfacepatch(patch.origin, face.orient, rsize, csize));
    }
}

// Shared corner heights join river slopes across several voxel steps without flattening large waterfalls.
// The sampler reads mounted material, so edits and streaming are reflected without consulting world generation.
template<class WaterAt, class SolidAt> static float naturalwatercornerheight(int x, int y, int z, WaterAt waterat, SolidAt solidat)
{
    float sum = 0;
    int count = 0, minimum = z, maximum = z;
    bool wet[4] = { false, false, false, false };
    for(int dy = 0; dy < 2; ++dy) for(int dx = 0; dx < 2; ++dx)
    {
        const int sx = x - dx, sy = y - dy;
        bool above = waterat(sx, sy, z + 64);
        if(above) return float(z); // A tall fall remains a vertical face.
        for(int top = z + 64; top >= z - 64; top -= 16)
        {
            const bool below = waterat(sx, sy, top - 1);
            if(below && !above)
            {
                minimum = min(minimum, top);
                maximum = max(maximum, top);
                sum += top;
                wet[dy * 2 + dx] = true;
                ++count;
                break;
            }
            above = below;
        }
    }
    if(!count || maximum - minimum > 64) return float(z);
    float height = sum / count;
    // At a dry bank, end the slope on the ground instead of leaving a vertical water curtain above it.
    // Search from the shared maximum, not the caller's height, so all adjacent patches agree.
    for(int dy = 0; dy < 2; ++dy) for(int dx = 0; dx < 2; ++dx) if(!wet[dy * 2 + dx])
    {
        const int sx = x - dx, sy = y - dy;
        for(int top = maximum; top >= minimum - 64; top -= 16)
        {
            if(!solidat(sx, sy, top - 1)) continue;
            height = min(height, float(top));
            break;
        }
    }
    return height;
}

template<class WaterAt> static float naturalwatercornerheight(int x, int y, int z, WaterAt waterat)
{
    return naturalwatercornerheight(x, y, z, waterat, [](int, int, int) { return false; });
}

// Fit each exposed side patch to the same corner heights as its upper and lower water surfaces.
// Returning false removes a collapsed internal riser in every render pass, including refraction masks.
template<class WaterAt, class CornerHeight>
static bool naturalwaterfallquad(int x, int y, int z, int orient, int length, int height, float offset,
                                 WaterAt waterat, CornerHeight cornerheight, vec *vertices)
{
    const int dim = dimension(orient), along = 1 - dim, sign = dimcoord(orient) ? 1 : -1;
    ivec a(x, y, z), b(a), inside(a), outside(a);
    b[along] += length;
    inside[along] += length / 2;
    outside[along] += length / 2;
    if(sign > 0) --inside[dim];
    else --outside[dim];
    const int top = z + height;
    int surface = top;
    bool upper = false;
    for(; surface <= top + 128; surface += 16)
    {
        if(waterat(inside.x, inside.y, surface - 1) && !waterat(inside.x, inside.y, surface)) { upper = true; break; }
    }
    const bool lower = waterat(outside.x, outside.y, z - 1) && !waterat(outside.x, outside.y, z);
    float atop = upper ? cornerheight(a.x, a.y, surface) : float(top),
          btop = upper ? cornerheight(b.x, b.y, surface) : float(top);
    if(surface != top) { atop = min(atop, float(top)); btop = min(btop, float(top)); }
    const float abottom = min(atop, lower ? cornerheight(a.x, a.y, z) : float(z)),
                bbottom = min(btop, lower ? cornerheight(b.x, b.y, z) : float(z));
    if(atop <= abottom && btop <= bbottom) return false;
    vertices[0] = vec(a.x, a.y, abottom);
    vertices[1] = vec(b.x, b.y, bbottom);
    vertices[2] = vec(b.x, b.y, btop);
    vertices[3] = vec(a.x, a.y, atop);
    for(int i = 0; i < 4; ++i) vertices[i][dim] += sign * offset;
    if((dim == 0) != (sign > 0)) swap(vertices[1], vertices[3]);
    return true;
}

#endif
