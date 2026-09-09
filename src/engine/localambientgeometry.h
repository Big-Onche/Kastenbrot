#ifndef LOCALAMBIENTGEOMETRY_H
#define LOCALAMBIENTGEOMETRY_H

// Geometry-only queries for both mounted and detached octrees. Do not use
// visibility-dependent collision caches: detached leaves have no render data.
struct localambientleafshape
{
    vec minimum, maximum;
    plane planes[12];
    int numplanes;

    localambientleafshape(const cube &c, int size) : numplanes(0)
    {
        clipplanes bounds;
        genclipbounds(c, ivec(0, 0, 0), size, bounds);
        minimum = vec(bounds.o).sub(bounds.r);
        maximum = vec(bounds.o).add(bounds.r);
        loopi(6) if(!flataxisface(c, i)) numplanes += genclipplane(c, i, bounds.v, planes + numplanes);
    }

    bool contains(const vec &point) const
    {
        loopi(3) if(point[i] < minimum[i] || point[i] >= maximum[i]) return false;
        loopi(numplanes) if(planes[i].dist(point) > 0) return false;
        return true;
    }

    bool roof(const vec &point, float &height) const
    {
        if(point.x < minimum.x || point.x >= maximum.x || point.y < minimum.y || point.y >= maximum.y) return false;
        float lower = max(point.z, minimum.z), upper = maximum.z;
        if(lower >= upper) return false;
        loopi(numplanes)
        {
            const plane &p = planes[i];
            const float offset = p.x * point.x + p.y * point.y + p.offset;
            if(p.z > 1e-6f) upper = min(upper, -offset / p.z);
            else if(p.z < -1e-6f) lower = max(lower, -offset / p.z);
            else if(offset > 0) return false;
        }
        if(lower >= upper) return false;
        height = lower;
        return true;
    }
};

static inline bool localambientleafroof(const cube &c, const ivec &point, const ivec &origin, int size, int &roof)
{
    if(isempty(c)) return false;
    if(isentirelysolid(c)) { roof = point.z; return true; }
    const localambientleafshape shape(c, size);
    float height;
    if(!shape.roof(vec(ivec(point).sub(origin)), height)) return false;
    roof = origin.z + int(ceilf(height));
    return true;
}

#endif
