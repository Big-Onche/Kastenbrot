// Direct draw consumer: no octree traversal and no temporary vtxarray.
static bool worldmeshshadowvisible(const worldmeshsection &section)
{
    if(!section.published) return false;
    switch(shadowmapping)
    {
        case SM_CASCADE: return (calcbbcsmsplits(section.minimum, section.maximum) & (1 << shadowside)) != 0;
        case SM_REFLECT: return calcbbrsmsplits(section.minimum, section.maximum) != 0;
        case SM_CUBEMAP: return (calcbbsidemask(section.minimum, section.maximum, shadoworigin, shadowradius, shadowbias) &
                                (1 << shadowside)) != 0;
        case SM_SPOT: return bbinsidespot(shadoworigin, shadowdir, shadowspot, section.minimum, section.maximum);
        default: return false;
    }
}

struct worldmeshcommand
{
    worldmeshbatchkey key;
    GLuint vbo, ebo;
    int section;
    uint first, count;

    worldmeshcommand() : vbo(0), ebo(0), section(0), first(0), count(0)
    {
    }

    worldmeshcommand(const worldmeshsection &mesh, const worldmeshdrawrange &range, int section)
        : key(range), vbo(mesh.vertices.buffer), ebo(mesh.indices.buffer), section(section),
          first(mesh.indices.offset + range.first * sizeof(uint)), count(range.count)
    {
    }

    bool compatible(const worldmeshcommand &b, bool depth) const
    {
        return vbo == b.vbo && ebo == b.ebo &&
               ((depth && key.renderclass != WORLDMESH_CUTOUT && b.key.renderclass != WORLDMESH_CUTOUT) || key == b.key);
    }
};

static bool worldmeshcommandorder(const worldmeshcommand &a, const worldmeshcommand &b)
{
    if(a.vbo != b.vbo) return a.vbo < b.vbo;
    if(a.ebo != b.ebo) return a.ebo < b.ebo;
    if(a.key == b.key) return a.section != b.section ? a.section < b.section : a.first < b.first;
    return a.key < b.key;
}

static bool worldmeshdepthorder(const worldmeshcommand &a, const worldmeshcommand &b)
{
    if(a.vbo != b.vbo) return a.vbo < b.vbo;
    if(a.ebo != b.ebo) return a.ebo < b.ebo;
    if(a.key.renderclass != b.key.renderclass) return a.key.renderclass < b.key.renderclass;
    if(a.key.renderclass == WORLDMESH_CUTOUT && !(a.key == b.key)) return a.key < b.key;
    return a.first < b.first;
}

// Rebuilt only when meshes are published, removed or reset. Entries refer to
// sections, never cubes. Refresh before dereferencing any cached section pointer.
static ullong worldmeshcommandgeneration = 0;
static vector<worldmeshsection *> worldmeshcommandsections;
static vector<worldmeshcommand> worldmeshopaquecommands, worldmeshalphacommands, worldmeshdepthcommands;
static vector<worldmeshcommand> worldmeshbackcommands, worldmeshrefractcommands;
static int worldmeshcutoutcommands = 0;
static vector<uchar> worldmeshcommandvisibility;
static vector<uchar> worldmeshcsmmasks;
static vector<GLsizei> worldmeshdrawcounts;
static vector<const GLvoid *> worldmeshdrawstarts;

void invalidateworldmeshcsm()
{
    worldmeshcsmmasks.setsize(0);
}

static void prepareworldmeshcommands()
{
    const ullong generation = getworldmeshgeneration();
    if(worldmeshcommandgeneration == generation) return;
    ZoneScopedN("WorldMesh/Rebuild submission catalogue");
    invalidateworldmeshcsm();
    worldmeshcommandsections.setsize(0);
    worldmeshopaquecommands.setsize(0);
    worldmeshalphacommands.setsize(0);
    worldmeshdepthcommands.setsize(0);
    worldmeshbackcommands.setsize(0);
    worldmeshrefractcommands.setsize(0);
    worldmeshcutoutcommands = 0;
    hashset<GLuint> vertexpages, indexpages;
    int numvertexpages = 0, numindexpages = 0;
    const vector<worldmeshsection *> &sections = getworldmeshsections();
    loopv(sections)
    {
        const worldmeshsection &section = *sections[i];
        if(!section.published || !section.vertices.buffer || !section.indices.buffer) continue;
        if(!vertexpages.access(section.vertices.buffer)) { vertexpages.add(section.vertices.buffer); ++numvertexpages; }
        if(!indexpages.access(section.indices.buffer)) { indexpages.add(section.indices.buffer); ++numindexpages; }
        const int sectionindex = worldmeshcommandsections.length();
        worldmeshcommandsections.add(sections[i]);
        loopvj(section.ranges)
        {
            const worldmeshdrawrange &range = section.ranges[j];
            if(!range.count || range.texture == DEFAULT_SKY) continue;
            worldmeshcommand command(section, range, sectionindex);
            if(range.alpha)
            {
                worldmeshalphacommands.add(command);
                const VSlot &slot = lookupvslot(range.texture);
                if(slot.alphaback) worldmeshbackcommands.add(command);
                if(slot.refractscale > 0) worldmeshrefractcommands.add(command);
            }
            else
            {
                if(range.renderclass == WORLDMESH_CUTOUT) ++worldmeshcutoutcommands;
                worldmeshopaquecommands.add(command);
                // smworld is depth-only for opaque geometry. Texture, texgen,
                // material and environment do not affect that shader.
                if(!worldmeshdepthcommands.empty() && worldmeshdepthcommands.last().section == sectionindex &&
                   worldmeshdepthcommands.last().compatible(command, true) &&
                   worldmeshdepthcommands.last().first + worldmeshdepthcommands.last().count * sizeof(uint) == command.first)
                    worldmeshdepthcommands.last().count += command.count;
                else worldmeshdepthcommands.add(command);
            }
        }
    }
    if(worldmeshopaquecommands.length() > 1) worldmeshopaquecommands.sort(worldmeshcommandorder);
    if(worldmeshdepthcommands.length() > 1) worldmeshdepthcommands.sort(worldmeshdepthorder);
    // Alpha and transmission retain section and range order. No global alpha sort.
    worldmeshcommandvisibility.setsize(0);
    worldmeshcommandvisibility.pad(worldmeshcommandsections.length());
    const int capacity = max(worldmeshopaquecommands.length(), max(worldmeshalphacommands.length(), worldmeshdepthcommands.length()));
    worldmeshdrawcounts.setsize(0);
    worldmeshdrawstarts.setsize(0);
    worldmeshdrawcounts.reserve(capacity);
    worldmeshdrawstarts.reserve(capacity);
    worldmeshcommandgeneration = generation;
    TracyPlot("WorldMesh/Terrain vertex pages", int64_t(numvertexpages));
    TracyPlot("WorldMesh/Terrain index pages", int64_t(numindexpages));
}

enum
{
    WORLDMESH_GBUFFER = 0, WORLDMESH_CSM, WORLDMESH_RSM, WORLDMESH_REFRACT, WORLDMESH_ALPHA, WORLDMESH_SHADOW, WORLDMESH_PASSES
};

struct worldmeshframestats
{
    ullong draws[WORLDMESH_PASSES], passindices[WORLDMESH_PASSES], indices, commands, cascade[4], cascadedepth[4],
           sections, ranges, sourceranges, maxranges, bindings, states, flushes, spans, merged, cutoutsubmitted;
    double millis[WORLDMESH_PASSES], cascademillis[4];
};

static worldmeshframestats worldmeshstats = {};

void beginworldmeshdrawstats()
{
    memset(&worldmeshstats, 0, sizeof(worldmeshstats));
}

void endworldmeshdrawstats()
{
#ifdef TRACY_ENABLE
    TracyPlot("WorldMesh/Enabled", int64_t(worldmeshpackets));
    ullong draws = 0;
    loopi(WORLDMESH_PASSES) draws += worldmeshstats.draws[i];
    TracyPlot("WorldMesh/Visible sections", int64_t(worldmeshstats.sections));
    TracyPlot("WorldMesh/Ranges visible", int64_t(worldmeshstats.ranges));
    TracyPlot("WorldMesh/Source ranges visible", int64_t(worldmeshstats.sourceranges));
    TracyPlot("WorldMesh/Draw calls", int64_t(draws));
    TracyPlot("WorldMesh/Submitted ranges", int64_t(worldmeshstats.commands));
    TracyPlot("WorldMesh/Cutout commands generated", int64_t(worldmeshcutoutcommands));
    TracyPlot("WorldMesh/Cutout commands submitted", int64_t(worldmeshstats.cutoutsubmitted));
    TracyPlot("WorldMesh/Flush count", int64_t(worldmeshstats.flushes));
    TracyPlot("WorldMesh/Draw spans", int64_t(worldmeshstats.spans));
    TracyPlot("WorldMesh/Merged contiguous spans", int64_t(worldmeshstats.merged));
    TracyPlot("WorldMesh/Indices", int64_t(worldmeshstats.indices));
    TracyPlot("WorldMesh/Avg ranges per visible section", worldmeshstats.sections ? double(worldmeshstats.ranges) / worldmeshstats.sections : 0.0);
    TracyPlot("WorldMesh/Max ranges per visible section", int64_t(worldmeshstats.maxranges));
    TracyPlot("WorldMesh/Buffer bindings", int64_t(worldmeshstats.bindings));
    TracyPlot("WorldMesh/Material changes", int64_t(worldmeshstats.states));
    TracyPlot("WorldMesh/GBuffer draws", int64_t(worldmeshstats.draws[WORLDMESH_GBUFFER]));
    TracyPlot("WorldMesh/CSM draws", int64_t(worldmeshstats.draws[WORLDMESH_CSM]));
    TracyPlot("WorldMesh/RSM draws", int64_t(worldmeshstats.draws[WORLDMESH_RSM]));
    TracyPlot("WorldMesh/Refract draws", int64_t(worldmeshstats.draws[WORLDMESH_REFRACT]));
    TracyPlot("WorldMesh/Alpha draws", int64_t(worldmeshstats.draws[WORLDMESH_ALPHA]));
    TracyPlot("WorldMesh/Other shadow draws", int64_t(worldmeshstats.draws[WORLDMESH_SHADOW]));
    TracyPlot("WorldMesh/GBuffer CPU ms", worldmeshstats.millis[WORLDMESH_GBUFFER]);
    TracyPlot("WorldMesh/CSM CPU ms", worldmeshstats.millis[WORLDMESH_CSM]);
    TracyPlot("WorldMesh/RSM CPU ms", worldmeshstats.millis[WORLDMESH_RSM]);
    TracyPlot("WorldMesh/Refract CPU ms", worldmeshstats.millis[WORLDMESH_REFRACT]);
    TracyPlot("WorldMesh/GBuffer indices", int64_t(worldmeshstats.passindices[WORLDMESH_GBUFFER]));
    TracyPlot("WorldMesh/CSM indices", int64_t(worldmeshstats.passindices[WORLDMESH_CSM]));
    TracyPlot("WorldMesh/RSM indices", int64_t(worldmeshstats.passindices[WORLDMESH_RSM]));
    TracyPlot("WorldMesh/Refract indices", int64_t(worldmeshstats.passindices[WORLDMESH_REFRACT]));
    #define WORLDMESH_CASCADE_STATS(n) \
        TracyPlot("WorldMesh/CSM cascade " #n " draws", int64_t(worldmeshstats.cascade[n])); \
        TracyPlot("WorldMesh/CSM cascade " #n " opaque draws", int64_t(worldmeshstats.cascadedepth[n])); \
        TracyPlot("WorldMesh/CSM cascade " #n " alpha draws", int64_t(worldmeshstats.cascade[n] - worldmeshstats.cascadedepth[n])); \
        TracyPlot("WorldMesh/CSM cascade " #n " CPU ms", worldmeshstats.cascademillis[n]);
    WORLDMESH_CASCADE_STATS(0);
    WORLDMESH_CASCADE_STATS(1);
    WORLDMESH_CASCADE_STATS(2);
    WORLDMESH_CASCADE_STATS(3);
    #undef WORLDMESH_CASCADE_STATS
#endif
}

static void setworldmeshbatchstate(renderstate &cur, int pass, const worldmeshbatchkey &key)
{
    GLOBALPARAMF(worldmeshcutout, key.renderclass == WORLDMESH_CUTOUT ? 1.0f : 0.0f);
    GLOBALPARAMF(worldmeshfoliage, key.twosided ? 1.0f : 0.0f);
    VSlot &vslot = lookupvslot(key.texture);
    Slot &slot = *vslot.slot;
    Texture *diffuse = slot.sts.empty() ? notexture : slot.sts[0].t;
    GLOBALPARAMF(worldmeshtexsize, float(diffuse->w), float(diffuse->h));
    changeslottmus(cur, pass, slot, vslot);
    if(slot.shader->type & SHADER_ENVMAP && !(slot.texmask & (1 << TEX_ENVMAP)))
    {
        const GLuint envmap = lookupenvmap(key.envmap);
        if(cur.textures[TEX_ENVMAP] != envmap)
        {
            glActiveTexture_(GL_TEXTURE0 + TEX_ENVMAP);
            glBindTexture(GL_TEXTURE_CUBE_MAP, cur.textures[TEX_ENVMAP] = envmap);
            glActiveTexture_(GL_TEXTURE0);
            cur.tmu = 0;
        }
    }
    changetexgen(cur, key.orient, slot, vslot);
    elementset element = {};
    element.layer = key.layer;
    struct meshshaderbatch
    {
        const elementset &es;
        VSlot &vslot;
        meshshaderbatch(const elementset &es, VSlot &vslot) : es(es), vslot(vslot)
        {
        }
    } batch(element, vslot);
    changeshader(cur, pass, batch);
}

// Toggle submission only for A/B measurements; packet layout and culling stay identical.
VAR(worldmeshmultidraw, 0, 1, 1);
VAR(worldmeshdebug, 0, 0, 1);

void renderworldmeshgeometry(int side, bool shadow, bool rsm, bool refractmask)
{
    ZoneScopedN("WorldMesh/Total");
    if(!worldmeshpackets) return;
    const Uint64 start = SDL_GetPerformanceCounter();
    prepareworldmeshcommands();
    const int pass = rsm ? RENDERPASS_RSM : shadow && side ? RENDERPASS_SMALPHA : RENDERPASS_GBUFFER;
    const bool depth = (shadow && !side && !rsm) || refractmask;
    const int statpass = refractmask ? WORLDMESH_REFRACT : rsm ? WORLDMESH_RSM :
                         shadow ? (shadowmapping == SM_CASCADE ? WORLDMESH_CSM : WORLDMESH_SHADOW) :
                         side ? WORLDMESH_ALPHA : WORLDMESH_GBUFFER;
    const vector<worldmeshcommand> &commands = refractmask ? worldmeshrefractcommands : side == 1 ? worldmeshbackcommands :
                                               side ? worldmeshalphacommands : depth ? worldmeshdepthcommands : worldmeshopaquecommands;
    if(commands.empty()) return;
    const bool attributes = !refractmask;
    {
        ZoneScopedN("WorldMesh/Cull sections");
        const bool cascade = (shadow || rsm) && shadowmapping == SM_CASCADE;
        if(cascade && worldmeshcsmmasks.length() != worldmeshcommandsections.length())
        {
            // calcbbcsmsplits already tests every cascade. Reuse that complete
            // mask across opaque/alpha draws until the next CSM setup.
            worldmeshcsmmasks.setsize(0);
            loopv(worldmeshcommandsections)
            {
                const worldmeshsection &section = *worldmeshcommandsections[i];
                worldmeshcsmmasks.add(calcbbcsmsplits(section.minimum, section.maximum));
            }
        }
        loopv(worldmeshcommandsections)
        {
            const worldmeshsection &section = *worldmeshcommandsections[i];
            const bool visible = cascade ? (worldmeshcsmmasks[i] & (1 << shadowside)) != 0 :
                                 shadow || rsm ? worldmeshshadowvisible(section) : worldmeshsectionvisible(section);
            worldmeshcommandvisibility[i] = visible;
            if(visible && statpass == WORLDMESH_GBUFFER && !drawtex)
            {
                ++worldmeshstats.sections;
                worldmeshstats.ranges += section.ranges.length();
                worldmeshstats.sourceranges += section.sourceranges;
                worldmeshstats.maxranges = max(worldmeshstats.maxranges, ullong(section.ranges.length()));
            }
        }
    }
    renderstate cur;
    cur.alphaing = side;
    cur.alphascale = -1;
    {
        ZoneScopedN("WorldMesh/Setup");
        if(!refractmask) setupgeom(cur);
        if(depth && !refractmask) SETSHADER(smworld);
        if(side == 1 && !shadow) glCullFace(GL_FRONT);
        enablevattribs(cur, attributes);
    }
    GLuint vbo = 0, ebo = 0;
    worldmeshbatchkey previouskey;
    bool materialset = false;
    const bool initialcull = glIsEnabled(GL_CULL_FACE) != 0;
    bool cullenabled = initialcull;
    ullong draws = 0;
    const worldmeshcommand *batch = NULL;
    worldmeshdrawcounts.setsize(0);
    worldmeshdrawstarts.setsize(0);
    auto flush = [&]()
    {
        if(!batch || worldmeshdrawcounts.empty()) return;
        const bool cull = initialcull && !batch->key.twosided;
        if(cullenabled != cull)
        {
            if(cull) glEnable(GL_CULL_FACE);
            else glDisable(GL_CULL_FACE);
            cullenabled = cull;
        }
        if(vbo != batch->vbo)
        {
            gle::bindvbo(vbo = batch->vbo);
            cur.vbuf = vbo;
            ++worldmeshstats.bindings;
            gle::vertexpointer(sizeof(vertex), (void *)offsetof(vertex, pos));
            if(attributes)
            {
                gle::normalpointer(sizeof(vertex), (void *)offsetof(vertex, norm), GL_BYTE);
                gle::texcoord0pointer(sizeof(vertex), (void *)offsetof(vertex, tc), GL_FLOAT, 3);
                gle::tangentpointer(sizeof(vertex), (void *)offsetof(vertex, tangent), GL_BYTE);
            }
        }
        if(ebo != batch->ebo)
        {
            gle::bindebo(ebo = batch->ebo);
            ++worldmeshstats.bindings;
        }
        if(!depth && (!materialset || !(previouskey == batch->key)))
        {
            setworldmeshbatchstate(cur, pass, batch->key);
            previouskey = batch->key;
            materialset = true;
            ++worldmeshstats.states;
        }
        if(depth && !refractmask)
        {
            if(batch->key.renderclass == WORLDMESH_CUTOUT)
            {
                VSlot &vslot = lookupvslot(batch->key.texture);
                Texture *diffuse = vslot.slot->sts.empty() ? notexture : vslot.slot->sts[0].t;
                GLOBALPARAMF(worldmeshtexsize, float(diffuse->w), float(diffuse->h));
                changeslottmus(cur, RENDERPASS_GBUFFER, *vslot.slot, vslot);
                changetexgen(cur, batch->key.orient, *vslot.slot, vslot);
                SETSHADER(smcutoutworld);
            }
            else SETSHADER(smworld);
        }
        const int count = worldmeshdrawcounts.length();
        ++worldmeshstats.flushes;
        worldmeshstats.spans += count;
        if(count > 1 && worldmeshmultidraw && glMultiDrawElements_)
        {
            glMultiDrawElements_(GL_TRIANGLES, worldmeshdrawcounts.getbuf(), GL_UNSIGNED_INT, worldmeshdrawstarts.getbuf(), count);
            ++draws;
            ++glde;
        }
        else loopi(count)
        {
            glDrawElements(GL_TRIANGLES, worldmeshdrawcounts[i], GL_UNSIGNED_INT, worldmeshdrawstarts[i]);
            ++draws;
            ++glde;
        }
        worldmeshdrawcounts.setsize(0);
        worldmeshdrawstarts.setsize(0);
    };
    {
        ZoneScopedN("WorldMesh/Submit batches");

        int submittedcommands = 0;

        loopv(commands)
        {
            const worldmeshcommand &command = commands[i];

            if(!worldmeshcommandvisibility[command.section]) continue;

            ++submittedcommands;
            if(command.key.renderclass == WORLDMESH_CUTOUT) ++worldmeshstats.cutoutsubmitted;

            if(batch && !batch->compatible(command, depth))
                flush();

            batch = &command;

            // Consecutive allocations/ranges with identical state need only one
            // indexed span, including inside a multi-draw call.
            if(!worldmeshdrawcounts.empty() &&
               (size_t)worldmeshdrawstarts.last() +
               size_t(worldmeshdrawcounts.last()) * sizeof(uint) == command.first)
            {
                worldmeshdrawcounts.last() += command.count;
                ++worldmeshstats.merged;
            }
            else
            {
                worldmeshdrawcounts.add(command.count);
                worldmeshdrawstarts.add((const GLvoid *)(size_t)command.first);
            }

            ++worldmeshstats.commands;
            worldmeshstats.indices += command.count;
            worldmeshstats.passindices[statpass] += command.count;
            xtravertsva += command.count;
        }

        flush();

        if(worldmeshdebug && statpass == WORLDMESH_GBUFFER && !drawtex)
        {
            static int lastreport = 0;
            if(totalmillis - lastreport >= 1000)
            {
                conoutf(CON_DEBUG, "WorldMesh: %d/%d commands, %d cutout generated, %d submitted; %d flushes, %d spans, %d merged",
                        submittedcommands, commands.length(), worldmeshcutoutcommands, int(worldmeshstats.cutoutsubmitted),
                        int(worldmeshstats.flushes), int(worldmeshstats.spans), int(worldmeshstats.merged));
                lastreport = totalmillis;
            }
        }

    }
    {
        ZoneScopedN("WorldMesh/Cleanup");
        disablevattribs(cur, attributes);
        disablevbuf(cur);
        if(side == 1 && !shadow) glCullFace(GL_BACK);
        if(!refractmask) cleanupgeom(cur);
        GLOBALPARAMF(worldmeshcutout, 0.0f);
        GLOBALPARAMF(worldmeshfoliage, 0.0f);
        if(cullenabled != initialcull) glEnable(GL_CULL_FACE);
    }
    const double millis = (SDL_GetPerformanceCounter() - start) * 1000.0 / SDL_GetPerformanceFrequency();
    worldmeshstats.draws[statpass] += draws;
    worldmeshstats.millis[statpass] += millis;
    if(statpass == WORLDMESH_CSM && shadowside >= 0 && shadowside < 4)
    {
        worldmeshstats.cascade[shadowside] += draws;
        if(depth) worldmeshstats.cascadedepth[shadowside] += draws;
        worldmeshstats.cascademillis[shadowside] += millis;
    }
}
