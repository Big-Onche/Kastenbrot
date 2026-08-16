// worldcontent.cpp: Kastenbrot world definitions and content rules

#ifdef WORLDIO_MODULE_IMPLEMENTATION

#ifndef STANDALONE
static bool parseworldpersistentid(const char *text, ullong &id)
{
    if(!text || !text[0]) return false;
    char *end = NULL;
    errno = 0;
    id = strtoull(text, &end, 10);
    return end && !*end && errno != ERANGE;
}
#endif

#ifndef STANDALONE
struct worldcubetexturekey
{
    ushort top, side, bottom;
    worldcubetexturekey(ushort top = 0, ushort side = 0, ushort bottom = 0) : top(top), side(side), bottom(bottom) {}
};

static inline uint hthash(const worldcubetexturekey &key) { return uint(key.top) ^ (uint(key.side) << 11) ^ (uint(key.bottom) << 22); }
static inline bool htcmp(const worldcubetexturekey &a, const worldcubetexturekey &b)
{
    return a.top == b.top && a.side == b.side && a.bottom == b.bottom;
}
#endif


static hashtable<worldcubetexturekey, int> worldcubetextureindexes(256);
vector<worldgencubetextures> worldgentextures;
int worldgrassscatter = -1, worldrosescatter = -1, worldtulipscatter = -1, worlddandelionscatter = -1;
static void updateleavesalpha();
static void setworldleavesalpha(cube *root, bool enabled);

int numworldcubes()
{
    return worldcubedefinitions.length();
}

static int validworldcubeindex(int index)
{
    if(worldcubedefinitions.inrange(index)) return index;
    return worldcubedefinitions.inrange(worlderrorcube) ? worlderrorcube : -1;
}

static int validworldobjectindex(int index)
{
    if(worldscatterdefinitions.inrange(index)) return index;
    return worldscatterdefinitions.inrange(worlderrorobject) ? worlderrorobject : -1;
}

int getworldcubeslot(int index)
{
    index = validworldcubeindex(index);
    return index >= 0 ? worldcubedefinitions[index]->slot : DEFAULT_GEOM;
}

int getworldcubefaceslot(int index, int orient)
{
    index = validworldcubeindex(index);
    if(index < 0) return DEFAULT_GEOM;
    const worlddefinition &type = *worldcubedefinitions[index];
    if(orient == WORLD_ORIENT_TOP) return type.slot;
    if(orient == WORLD_ORIENT_BOTTOM) return type.bottomslot;
    return type.sideslot;
}

static int getworldcubebytextures(const ushort *textures)
{
    loopi(4) if(textures[i] != textures[0]) return -1;
    int *index = worldcubetextureindexes.access(worldcubetexturekey(textures[O_TOP], textures[0], textures[O_BOTTOM]));
    return index ? *index : -1;
}

int getworldcubeindex(int slot)
{
    if(worldcubedefinitions.inrange(worlderrorcube))
    {
        const worlddefinition &error = *worldcubedefinitions[worlderrorcube];
        if(error.slot == slot || error.sideslot == slot || error.bottomslot == slot) return worlderrorcube;
    }
    loopv(worldcubedefinitions)
    {
        if(i == worlderrorcube) continue;
        const worlddefinition &type = *worldcubedefinitions[i];
        if(type.slot == slot || type.sideslot == slot || type.bottomslot == slot) return i;
    }
    return worldcubedefinitions.inrange(worlderrorcube) ? worlderrorcube : -1;
}

int getworldcubeindexat(const ivec &position, int orient)
{
    // Block identity belongs to the complete texture set, not the face that happened to be targeted.
    (void)orient;
    ivec origin;
    int size;
    const cube &c = lookupcube(position, 0, origin, size);
    return validworldcubeindex(getworldcubebytextures(c.texture));
}

ullong getworldcubepersistentid(int index)
{
    index = validworldcubeindex(index);
    return index >= 0 ? worldcubedefinitions[index]->persistentid : 0;
}

int getworldcubepersistentindex(ullong id, bool warn)
{
    int *index = worldcubepersistentindexes.access(worldpersistentkey(id));
    if(index) return *index;
    if(warn) conoutf(CON_WARN, "unknown persistent world cube ID " WORLD_ULL_FORMAT "; using error cube", id);
    return validworldcubeindex(worlderrorcube);
}

int getworldcubetextureslotat(const ivec &position, int orient)
{
    ivec origin;
    int size;
    const cube &c = lookupcube(position, 0, origin, size);
    return c.texture[clamp(orient, 0, 5)];
}

bool isworldcubesolidat(const ivec &position)
{
    ivec origin;
    int size;
    const cube &c = lookupcube(position, 0, origin, size);
    return !isempty(c) && isentirelysolid(c);
}

const char *getworldcubename(int index)
{
    index = validworldcubeindex(index);
    return index >= 0 ? worldcubedefinitions[index]->id : "";
}

int getworldcubeitem(int index)
{
    index = validworldcubeindex(index);
    return index >= 0 ? worldcubedefinitions[index]->item : -1;
}

bool isworldcubepushable(int index, int toolitem)
{
    if(!inventoryitemdefinitions.inrange(toolitem) || !inventoryitemdefinitions[toolitem]->hastool) return false;
    string tagid;
    formatstring(tagid, "pushable_with_%s", inventoryitemdefinitions[toolitem]->tooltype);
    const int tag = finditemtag(tagid);
    return tag >= 0 && itemhastag(getworldcubeitem(index), tag);
}

const char *getworldcubetexture(int index, int face)
{
    static string texturepath;
    index = validworldcubeindex(index);
    if(index < 0) return "";
    worlddefinition &type = *worldcubedefinitions[index];
    const char *texture = type.cubetexture;
    if(face == WORLD_CUBE_SIDE && type.sidetexture[0]) texture = type.sidetexture;
    else if(face == WORLD_CUBE_BOTTOM)
        texture = type.bottomtexture[0] ? type.bottomtexture
                : type.sidetexture[0] ? type.sidetexture
                : type.cubetexture;
    formatstring(texturepath, "media/texture/%s", texture);
    return texturepath;
}

int numworldscatters()
{
    return worldscatterdefinitions.length();
}

const char *getworldscattername(int index)
{
    index = validworldobjectindex(index);
    return index >= 0 ? worldscatterdefinitions[index]->id : "";
}

ullong getworldscatterpersistentid(int index)
{
    index = validworldobjectindex(index);
    return index >= 0 ? worldscatterdefinitions[index]->persistentid : 0;
}

int getworldscatterpersistentindex(ullong id, bool warn)
{
    int *index = worldscatterpersistentindexes.access(worldpersistentkey(id));
    if(index) return *index;
    if(warn) conoutf(CON_WARN, "unknown persistent world object ID " WORLD_ULL_FORMAT "; using error object", id);
    return validworldobjectindex(worlderrorobject);
}

const char *getworldscattermodel(int index)
{
    index = validworldobjectindex(index);
    return index >= 0 ? worldscatterdefinitions[index]->model : "";
}

const char *getworldscattericon(int index)
{
    static string iconpath;
    index = validworldobjectindex(index);
    if(index < 0) return "";
    const worlddefinition &type = *worldscatterdefinitions[index];
    if(type.modelicon[0]) return type.modelicon;
    formatstring(iconpath, "media/model/%s/diffuse.png", type.model);
    return iconpath;
}

bool isworldtorch(int index)
{
    return worldscatterdefinitions.inrange(index) && worldscatterdefinitions[index]->lightradius > 0;
}

int getworldscatteritem(int index)
{
    index = validworldobjectindex(index);
    return index >= 0 ? worldscatterdefinitions[index]->item : -1;
}

bool isworldplaceable(int index)
{
    return worldscatterdefinitions.inrange(index) && worldscatterdefinitions[index]->placeable;
}

int numworldplaceables()
{
    int count = 0;
    loopv(worldscatterdefinitions) if(worldscatterdefinitions[i]->placeable) ++count;
    return count;
}

int getworldplaceableindex(int index)
{
    loopv(worldscatterdefinitions) if(worldscatterdefinitions[i]->placeable && index-- == 0) return i;
    return -1;
}

int numinventoryitems()
{
    return inventoryitemdefinitions.length();
}

const char *getinventoryitemname(int index)
{
    return inventoryitemdefinitions.inrange(index) ? inventoryitemdefinitions[index]->name : "";
}

const char *getinventoryitemid(int index)
{
    return inventoryitemdefinitions.inrange(index) ? inventoryitemdefinitions[index]->id : "";
}

ullong getinventoryitempersistentid(int index)
{
    return inventoryitemdefinitions.inrange(index) ? inventoryitemdefinitions[index]->persistentid : 0;
}

int getinventoryitempersistentindex(ullong id, bool warn)
{
    int *index = inventoryitempersistentindexes.access(worldpersistentkey(id));
    if(index) return *index;
    if(warn) conoutf(CON_WARN, "unknown persistent inventory item ID " WORLD_ULL_FORMAT "; using error item", id);
    return inventoryitemdefinitions.inrange(worlderroritem) ? worlderroritem : -1;
}

bool getworldcubefall(int index)
{
    index = validworldcubeindex(index);
    return index >= 0 && worldcubedefinitions[index]->fall;
}

int getworldcubesupportdistance(int index)
{
    return worldcubedefinitions.inrange(index) && worldcubedefinitions[index]->hassupport ? worldcubedefinitions[index]->supportdistance : 0;
}

bool getworldcubesupportdecay(int index)
{
    return worldcubedefinitions.inrange(index) && worldcubedefinitions[index]->hassupport && worldcubedefinitions[index]->supportdecay;
}

bool getworldcubesupportpersistentonplace(int index)
{
    return worldcubedefinitions.inrange(index) && worldcubedefinitions[index]->hassupport &&
           worldcubedefinitions[index]->supportpersistentonplace;
}

int getinventoryitemindex(const char *id)
{
    worlddefinition *item = findinventoryitem(id);
    return item ? inventoryitemdefinitions.find(item) : -1;
}

int getinventoryitemmaxstack(int index)
{
    return inventoryitemdefinitions.inrange(index) ? inventoryitemdefinitions[index]->maxstack : 0;
}

bool isinventoryfood(int index)
{
    return inventoryitemdefinitions.inrange(index) && inventoryitemdefinitions[index]->hasfood;
}

float getinventoryfoodhealth(int index)
{
    return isinventoryfood(index) ? inventoryitemdefinitions[index]->foodhealth : 0.0f;
}

int getinventoryfoodtime(int index)
{
    return isinventoryfood(index) ? inventoryitemdefinitions[index]->foodtime : 0;
}

const char *getinventoryitemtexture(int index)
{
    return inventoryitemdefinitions.inrange(index) ? inventoryitemdefinitions[index]->texture : "";
}

const char *getinventoryitemicon(int index)
{
    static string iconpath;
    if(!inventoryitemdefinitions.inrange(index)) return "";
    const worlddefinition &item = *inventoryitemdefinitions[index];
    if(item.icon[0]) return item.icon;
    if(item.texture[0])
    {
        formatstring(iconpath, "media/texture/%s", item.texture);
        return iconpath;
    }
    loopv(worldcubedefinitions) if(worldcubedefinitions[i]->item == index)
    {
        formatstring(iconpath, "media/texture/%s", worldcubedefinitions[i]->cubetexture);
        return iconpath;
    }
    loopv(worldscatterdefinitions) if(worldscatterdefinitions[i]->item == index)
    {
        return getworldscattericon(i);
    }
    return "";
}

float getinventoryitemworldsize(int index)
{
    return inventoryitemdefinitions.inrange(index) ? inventoryitemdefinitions[index]->worldsize : 1.0f;
}

bool getinventoryitemheldflipx(int index)
{
    return inventoryitemdefinitions.inrange(index) && inventoryitemdefinitions[index]->heldflipx;
}

bool getinventoryitemheldflipy(int index)
{
    return inventoryitemdefinitions.inrange(index) && inventoryitemdefinitions[index]->heldflipy;
}

float getinventoryitemheldsize(int index)
{
    return inventoryitemdefinitions.inrange(index) ? inventoryitemdefinitions[index]->heldsize / 100.0f : 1.0f;
}

int getworlditemtype(int item)
{
    loopv(worldcubedefinitions) if(worldcubedefinitions[i]->item == item) return WORLD_ITEM_CUBE;
    loopv(worldscatterdefinitions) if(worldscatterdefinitions[i]->item == item)
        return worldscatterdefinitions[i]->placeable ? WORLD_ITEM_PLACEABLE : WORLD_ITEM_SCATTER;
    return WORLD_ITEM_NONE;
}

int getworlditemindex(int item)
{
    loopv(worldcubedefinitions) if(worldcubedefinitions[i]->item == item) return i;
    loopv(worldscatterdefinitions) if(worldscatterdefinitions[i]->item == item) return i;
    return -1;
}

float getworlditemlightradius(int item)
{
    loopv(worldscatterdefinitions) if(worldscatterdefinitions[i]->item == item) return worldscatterdefinitions[i]->lightradius;
    return 0.0f;
}

static vector<worlddropdefinition> &worldobjectdrops(int type, int index)
{
    static vector<worlddropdefinition> empty;
    if(type == WORLD_ITEM_CUBE && worldcubedefinitions.inrange(index)) return worldcubedefinitions[index]->drops;
    if((type == WORLD_ITEM_SCATTER || type == WORLD_ITEM_PLACEABLE) && worldscatterdefinitions.inrange(index))
        return worldscatterdefinitions[index]->drops;
    return empty;
}

int getworldobjectdropcount(int type, int index)
{
    vector<worlddropdefinition> &drops = worldobjectdrops(type, index);
    if(!drops.empty())
    {
        loopv(drops) if(drops[i].item >= 0) return drops.length();
        return 0;
    }
    int item = type == WORLD_ITEM_CUBE && worldcubedefinitions.inrange(index) ? worldcubedefinitions[index]->item
             : (type != WORLD_ITEM_CUBE && worldscatterdefinitions.inrange(index) ? worldscatterdefinitions[index]->item : -1);
    return item >= 0 ? 1 : 0;
}

bool getworldobjectdrop(int type, int index, int drop, int &item, int &mincount, int &maxcount, float &chance)
{
    vector<worlddropdefinition> &drops = worldobjectdrops(type, index);
    if(!drops.empty())
    {
        if(!drops.inrange(drop)) return false;
        const worlddropdefinition &entry = drops[drop];
        item = entry.item;
        mincount = entry.mincount;
        maxcount = entry.maxcount;
        chance = entry.chance;
        return item >= 0;
    }
    if(drop != 0) return false;
    item = type == WORLD_ITEM_CUBE && worldcubedefinitions.inrange(index) ? worldcubedefinitions[index]->item
         : (type != WORLD_ITEM_CUBE && worldscatterdefinitions.inrange(index) ? worldscatterdefinitions[index]->item : -1);
    mincount = maxcount = 1;
    chance = 1.0f;
    return item >= 0;
}

bool worldcellacceptswater(const ivec &position)
{
    const cube &c = lookupcube(ivec(position).add(WORLD_BLOCK_SIZE / 2));
    const int material = c.material&MATF_VOLUME;
    return isempty(c) && (material == MAT_AIR || material == MAT_WATER);
}

bool worldcellhaswater(const ivec &position)
{
    const cube &c = lookupcube(ivec(position).add(WORLD_BLOCK_SIZE / 2));
    return (c.material&MATF_VOLUME) == MAT_WATER;
}

int worldcellmaterial(const ivec &position)
{
    return lookupcube(ivec(position).add(WORLD_BLOCK_SIZE / 2)).material;
}

bool worldcellsolid(const ivec &position)
{
    const cube &c = lookupcube(ivec(position).add(WORLD_BLOCK_SIZE / 2));
    return !isempty(c);
}

void worldwaterchanged(const ivec &minimum, const ivec &maximum)
{
    if(!worldroot) return;
    selinfo localminimum, localmaximum;
    localminimum.o = minimum;
    localmaximum.o = maximum;
    worldselectiontolocal(localminimum);
    worldselectiontolocal(localmaximum);
    changedgeometry(localminimum.o, localmaximum.o);
}

VARFP(leavesalpha, 0, 1, 1, updateleavesalpha());

static bool isworldleaftexture(const cube &c)
{
    if(c.children || isempty(c)) return false;
    const int texture = c.texture[0];
    worlddefinition *leaves = findworldcube("leaves"), *needles = findworldcube("needles");
    const bool foliage = (leaves && texture == leaves->slot) || (needles && texture == needles->slot);
    if(!foliage) return false;
    loopi(6) if(c.texture[i] != texture) return false;
    return true;
}

bool isworldleafcube(const cube &c)
{
    return leavesalpha != 0 && isworldleaftexture(c);
}

static int getworldscatteridindex(const char *id)
{
    worlddefinition *type = findworldscatter(id);
    return type ? worldscatterdefinitions.find(type) : worlderrorobject;
}

void worldreset()
{
    game::cleanupitemsprites();
    resetworlddefinitionregistry();
    worldcubetextureindexes.clear();
    worldgentextures.shrink(0);
    worldgrassscatter = worldrosescatter = worldtulipscatter = worlddandelionscatter = -1;
}

COMMAND(worldreset, "");

bool getworldfurnaceconfig(int item, int &inputslots, int &inputlimit)
{
    if(inventoryitemdefinitions.inrange(item) && inventoryitemdefinitions[item]->hasfurnace)
    {
        inputslots = inventoryitemdefinitions[item]->furnaceinputslots;
        inputlimit = inventoryitemdefinitions[item]->furnaceinputlimit;
        return true;
    }
    inputslots = inputlimit = 0;
    return false;
}

static worlddefinition *getworldobjectdefinition(int type, int index)
{
    if(type == WORLD_ITEM_CUBE) return worldcubedefinitions.inrange(index) ? worldcubedefinitions[index] : NULL;
    if(type == WORLD_ITEM_SCATTER || type == WORLD_ITEM_PLACEABLE)
        return worldscatterdefinitions.inrange(index) ? worldscatterdefinitions[index] : NULL;
    return NULL;
}

bool isinventorytool(int index)
{
    return inventoryitemdefinitions.inrange(index) && inventoryitemdefinitions[index]->hastool;
}

const char *getinventorytooltype(int index)
{
    return isinventorytool(index) ? inventoryitemdefinitions[index]->tooltype : "";
}

int getinventorytooltier(int index)
{
    return isinventorytool(index) ? inventoryitemdefinitions[index]->tooltier : 0;
}

float getinventorytoolspeed(int index)
{
    return isinventorytool(index) ? inventoryitemdefinitions[index]->toolspeed : 1.0f;
}

int getinventorytoolmaxdurability(int index)
{
    return isinventorytool(index) ? inventoryitemdefinitions[index]->maxdurability : 0;
}

bool getworldchestconfig(int item, int &slots)
{
    if(inventoryitemdefinitions.inrange(item) && inventoryitemdefinitions[item]->haschest)
    {
        slots = inventoryitemdefinitions[item]->chestslots;
        return true;
    }
    slots = 0;
    return false;
}

int getinventorytoolcornerpush(int index)
{
    return isinventorytool(index) ? inventoryitemdefinitions[index]->toolcornerpush : TOOL_CORNER_PUSH_NONE;
}

float getinventorytooldamage(int index)
{
    return isinventorytool(index) ? inventoryitemdefinitions[index]->tooldamage : 1.0f;
}

float getworldobjecthardness(int type, int index)
{
    worlddefinition *definition = getworldobjectdefinition(type, index);
    return definition && definition->hasmining ? max(definition->hardness, 0.01f) : 1.0f;
}

const char *getworldobjectpreferredtool(int type, int index)
{
    worlddefinition *definition = getworldobjectdefinition(type, index);
    return definition && definition->hasmining ? definition->preferredtool : "";
}

int getworldobjectrequiredtier(int type, int index)
{
    worlddefinition *definition = getworldobjectdefinition(type, index);
    return definition && definition->hasmining ? definition->requiredtier : 0;
}

int getworldobjecttoolwear(int type, int index)
{
    worlddefinition *definition = getworldobjectdefinition(type, index);
    return definition && definition->hasmining ? definition->toolwear : 1;
}

bool isworldobjecthandbreakable(int type, int index)
{
    worlddefinition *definition = getworldobjectdefinition(type, index);
    return !definition || !definition->hasmining || definition->handbreakable;
}

static int loadworldtextureslot(const char *path, float texsize, bool alpha)
{
    const char *texture = escapestring(path);
    string command;
    if(alpha) formatstring(command, "setshader leafworld; texture 0 %s; texture a %s; texscale %.9g; texalpha 1 1", texture, texture, texsize);
    else formatstring(command, "setshader stdworld; texture 0 %s; texscale %.9g", texture, texsize);
    execute(command);
    return slots.last()->variants->index;
}

static bool canloadworldtexture(const char *path)
{
    if(!path || !path[0]) return false;
    defformatstring(filename, "media/texture/%s", path);
    return textureload(filename, 3, true, false) != notexture;
}

static void validateworlderrorfallback(bool assets)
{
    worlddefinition *error = findworlddefinition("error");
    if(!error || !error->hasitem || !error->hascube || !error->scatter || !error->placeable)
        fatal("world startup failed: worlddef \"error\" must contain item, cube, scatter, and placeable components");
    worlderroritem = inventoryitemdefinitions.find(error);
    worlderrorcube = worldcubedefinitions.find(error);
    worlderrorobject = worldscatterdefinitions.find(error);
    if(!assets) return;

    if(!canloadworldtexture(error->cubetexture))
        fatal("world startup failed: error cube texture media/texture/%s could not be loaded", error->cubetexture);
    if(error->sidetexture[0] && !canloadworldtexture(error->sidetexture))
        fatal("world startup failed: error cube side texture media/texture/%s could not be loaded", error->sidetexture);
    if(error->bottom[0] && !findworldcube(error->bottom) && !canloadworldtexture(error->bottom))
        fatal("world startup failed: error cube bottom texture media/texture/%s could not be loaded", error->bottom);

    error->mapmodel = registermapmodelpath(error->model);
    if(error->mapmodel < 0 || !loadmapmodel(error->mapmodel))
        fatal("world startup failed: error model media/model/%s could not be loaded", error->model);
    defformatstring(modeltexture, "media/model/%s/diffuse.png", error->model);
    if(textureload(modeltexture, 3, true, false) == notexture)
        fatal("world startup failed: error model texture %s could not be loaded", modeltexture);
}

static bool findworldscatterimage(const char *model, const char *basename, string &imagepath)
{
    defformatstring(directory, "media/model/%s", model);
    vector<char *> files;
    listfiles(directory, NULL, files);
    files.sort();

    const size_t baselen = strlen(basename);
    loopv(files)
    {
        const char *filename = files[i];
        if(cubecasecmp(filename, basename, baselen) || filename[baselen] != '.' || !filename[baselen + 1]) continue;

        defformatstring(candidate, "%s/%s", directory, filename);
        if(textureload(candidate, 3, true, false) == notexture) continue;
        copystring(imagepath, candidate);
        files.deletecontents();
        return true;
    }
    files.deletecontents();
    return false;
}

static void resolveworldscattericon(worlddefinition &type)
{
    type.modelicon[0] = '\0';
    if(findworldscatterimage(type.model, "logo", type.modelicon)) return;
    if(findworldscatterimage(type.model, "diffuse", type.modelicon)) return;
    formatstring(type.modelicon, "media/model/%s/diffuse.png", type.model);
}

struct worldscatterrenderdefinition
{
    string texture;
    vec center, radius;

    worldscatterrenderdefinition() : center(0, 0, WORLD_BLOCK_SIZE * 0.5f), radius(WORLD_BLOCK_SIZE * 0.5f) { texture[0] = '\0'; }
};

static vector<worldscatterrenderdefinition> worldscatterrenderdefinitions;

static void resolveworldscatterrenderdata(int index, worlddefinition &type)
{
    while(worldscatterrenderdefinitions.length() <= index) worldscatterrenderdefinitions.add();
    worldscatterrenderdefinition &render = worldscatterrenderdefinitions[index];
    render.texture[0] = '\0';
    if(!findworldscatterimage(type.model, "diffuse", render.texture) && !findworldscatterimage(type.model, "logo", render.texture))
        formatstring(render.texture, "media/model/%s/diffuse.png", type.model);

    model *m = type.mapmodel >= 0 ? loadmapmodel(type.mapmodel) : NULL;
    if(m) m->boundbox(render.center, render.radius);
    else
    {
        render.center = vec(0, 0, WORLD_BLOCK_SIZE * 0.5f);
        render.radius = vec(WORLD_BLOCK_SIZE * 0.5f);
    }
}

static bool buildworldcubetextureindexes()
{
    bool valid = true;
    worldcubetextureindexes.clear();
    loopv(worldcubedefinitions)
    {
        const worlddefinition &definition = *worldcubedefinitions[i];
        const worldcubetexturekey key(definition.slot, definition.sideslot, definition.bottomslot);
        int *previous = worldcubetextureindexes.access(key);
        if(previous)
        {
            conoutf(CON_ERROR, "world cubes \"%s\" and \"%s\" have indistinguishable runtime textures; persistent identity would be ambiguous",
                    worldcubedefinitions[*previous]->id, definition.id);
            valid = false;
        }
        else worldcubetextureindexes.access(key, i);
    }
    return valid;
}

static bool loadworlddefinitions(bool assets = true)
{
    worldreset();
    worldscatterrenderdefinitions.setsize(0);
    if(!execfile("config/world.cfg", false))
    {
        conoutf(CON_ERROR, "could not load config/world.cfg");
        return false;
    }

    if(!resolveworlddefinitionregistry()) return false;
    validateworlderrorfallback(assets);
    if(!reloadrecipes(true)) return false;

    if(!assets)
    {
        conoutf(CON_DEBUG, "loaded %d inventory item, %d world cube, and %d world object server definitions",
                inventoryitemdefinitions.length(), worldcubedefinitions.length(), worldscatterdefinitions.length());
        return true;
    }

    execute("texturereset; texsky; setshader stdworld");
    worlddefinition &errorcube = *worldcubedefinitions[worlderrorcube];
    errorcube.slot = loadworldtextureslot(errorcube.cubetexture, errorcube.texsize, false);
    errorcube.sideslot = errorcube.sidetexture[0]
                       ? loadworldtextureslot(errorcube.sidetexture, errorcube.texsize, false)
                       : errorcube.slot;
    loopv(worldcubedefinitions)
    {
        if(i == worlderrorcube) continue;
        worlddefinition &type = *worldcubedefinitions[i];
        type.errorfallback = false;
        const bool alpha = !cubecasecmp(type.id, "leaves") || !cubecasecmp(type.id, "needles");
        if(canloadworldtexture(type.cubetexture)) type.slot = loadworldtextureslot(type.cubetexture, type.texsize, alpha);
        else
        {
            conoutf(CON_ERROR, "world cube %s could not load texture %s; using error cube", type.id, type.cubetexture);
            type.errorfallback = true;
            copystring(type.cubetexture, errorcube.cubetexture);
            type.slot = errorcube.slot;
        }
        if(!type.sidetexture[0]) type.sideslot = type.slot;
        else if(canloadworldtexture(type.sidetexture)) type.sideslot = loadworldtextureslot(type.sidetexture, type.texsize, alpha);
        else
        {
            conoutf(CON_ERROR, "world cube %s could not load side texture %s; using error cube", type.id, type.sidetexture);
            type.errorfallback = true;
            copystring(type.sidetexture, errorcube.sidetexture[0] ? errorcube.sidetexture : errorcube.cubetexture);
            type.sideslot = errorcube.sideslot;
        }
    }
    worlddefinition *errorbottomtype = errorcube.bottom[0] ? findworldcube(errorcube.bottom) : NULL;
    if(errorbottomtype)
    {
        errorcube.bottomslot = errorbottomtype->slot;
        copystring(errorcube.bottomtexture, errorbottomtype->cubetexture);
    }
    else if(errorcube.bottom[0])
    {
        errorcube.bottomslot = loadworldtextureslot(errorcube.bottom, errorcube.texsize, false);
        copystring(errorcube.bottomtexture, errorcube.bottom);
    }
    else
    {
        errorcube.bottomslot = errorcube.sideslot;
        copystring(errorcube.bottomtexture, errorcube.sidetexture[0] ? errorcube.sidetexture : errorcube.cubetexture);
    }
    loopv(worldcubedefinitions)
    {
        if(i == worlderrorcube) continue;
        worlddefinition &type = *worldcubedefinitions[i];
        worlddefinition *bottomtype = type.bottom[0] ? findworldcube(type.bottom) : NULL;
        if(bottomtype)
        {
            type.bottomslot = bottomtype->slot;
            copystring(type.bottomtexture, bottomtype->cubetexture);
        }
        else if(type.bottom[0])
        {
            const bool alpha = !cubecasecmp(type.id, "leaves") || !cubecasecmp(type.id, "needles");
            if(canloadworldtexture(type.bottom))
            {
                type.bottomslot = loadworldtextureslot(type.bottom, type.texsize, alpha);
                copystring(type.bottomtexture, type.bottom);
            }
            else
            {
                conoutf(CON_ERROR, "world cube %s could not load bottom texture %s; using error cube", type.id, type.bottom);
                type.errorfallback = true;
                type.bottomslot = errorcube.sideslot;
                copystring(type.bottomtexture, errorcube.sidetexture[0] ? errorcube.sidetexture : errorcube.cubetexture);
            }
        }
        else
        {
            type.bottomslot = type.sideslot;
            copystring(type.bottomtexture, type.sidetexture[0] ? type.sidetexture : type.cubetexture);
        }
    }
    loopv(worldcubedefinitions)
    {
        worlddefinition &type = *worldcubedefinitions[i];
        if(!type.errorfallback) continue;
        copystring(type.cubetexture, errorcube.cubetexture);
        copystring(type.sidetexture, errorcube.sidetexture);
        copystring(type.bottomtexture, errorcube.bottomtexture);
        type.slot = errorcube.slot;
        type.sideslot = errorcube.sideslot;
        type.bottomslot = errorcube.bottomslot;
    }

    if(!buildworldcubetextureindexes()) return false;

    loopv(worldcubedefinitions)
    {
        const worlddefinition &type = *worldcubedefinitions[i];
        worldgentextures.add(worldgencubetextures(type.id, type.slot, type.sideslot, type.bottomslot));
    }
    loopv(worldscatterdefinitions)
    {
        worlddefinition &type = *worldscatterdefinitions[i];
        if(i == worlderrorobject)
        {
            resolveworldscattericon(type);
            resolveworldscatterrenderdata(i, type);
            continue;
        }
        type.mapmodel = registermapmodelpath(type.model);
        if(type.mapmodel < 0 || !loadmapmodel(type.mapmodel))
        {
            conoutf(CON_ERROR, "world object %s could not load model %s; using error model", type.id, type.model);
            copystring(type.model, worldscatterdefinitions[worlderrorobject]->model);
            type.mapmodel = worldscatterdefinitions[worlderrorobject]->mapmodel;
        }
        resolveworldscattericon(type);
        resolveworldscatterrenderdata(i, type);
    }
    worldgrassscatter = getworldscatteridindex("grass_scatter");
    worldrosescatter = getworldscatteridindex("rose");
    worldtulipscatter = getworldscatteridindex("tulip");
    worlddandelionscatter = getworldscatteridindex("dandelion");
    setworldleavesalpha(worldroot, leavesalpha != 0);
    game::preloaditemsprites();
    conoutf(CON_DEBUG, "loaded %d inventory item, %d world cube, and %d world object definitions",
            inventoryitemdefinitions.length(), worldcubedefinitions.length(), worldscatterdefinitions.length());
    return true;
}

void initworlddefinitions()
{
    if(!loadworlddefinitions(true))
        fatal("world startup failed: config/world.cfg contains invalid definitions; see the preceding error for details");
}

void initserverworlddefinitions()
{
    if(!loadworlddefinitions(false))
        fatal("server startup failed: config/world.cfg contains invalid definitions; see the preceding error for details");
}

ICOMMAND(worldload, "", (), intret(loadworlddefinitions(true) ? 1 : 0));


#endif

#ifdef WORLDIO_STANDALONE_CONTENT_IMPLEMENTATION


static void resetserverworlddefinitions() { resetworlddefinitionregistry(); }
COMMANDN(worldreset, resetserverworlddefinitions, "");

void initserverworlddefinitions()
{
    resetserverworlddefinitions();
    if(!execfile("config/world.cfg", false)) fatal("server startup failed: could not load config/world.cfg");
    if(!resolveworlddefinitionregistry()) fatal("server startup failed: invalid world definitions");
    if(!reloadrecipes(true)) fatal("server startup failed: invalid recipes");
    conoutf("loaded %d inventory item, %d world cube, and %d world object server definitions",
            inventoryitemdefinitions.length(), worldcubedefinitions.length(), worldscatterdefinitions.length());
}

void initworlddefinitions() { initserverworlddefinitions(); }

ICOMMAND(worldload, "", (),
{
    initserverworlddefinitions();
    intret(1);
});

int numinventoryitems() { return inventoryitemdefinitions.length(); }

const char *getinventoryitemid(int index)
{
    return inventoryitemdefinitions.inrange(index) ? inventoryitemdefinitions[index]->id : "";
}

ullong getinventoryitempersistentid(int index)
{
    return inventoryitemdefinitions.inrange(index) ? inventoryitemdefinitions[index]->persistentid : 0;
}

int getinventoryitempersistentindex(ullong id, bool warn)
{
    int *index = inventoryitempersistentindexes.access(worldpersistentkey(id));
    if(index) return *index;
    if(warn) conoutf(CON_WARN, "unknown persistent inventory item ID " WORLD_ULL_FORMAT "; using error item", id);
    return inventoryitemdefinitions.inrange(worlderroritem) ? worlderroritem : -1;
}

int getinventoryitemindex(const char *id)
{
    worlddefinition *item = findinventoryitem(id);
    return item ? inventoryitemdefinitions.find(item) : -1;
}

int getinventoryitemmaxstack(int index)
{
    return inventoryitemdefinitions.inrange(index) ? inventoryitemdefinitions[index]->maxstack : 0;
}

bool isinventoryfood(int index) { return inventoryitemdefinitions.inrange(index) && inventoryitemdefinitions[index]->hasfood; }
float getinventoryfoodhealth(int index) { return isinventoryfood(index) ? inventoryitemdefinitions[index]->foodhealth : 0.0f; }
int getinventoryfoodtime(int index) { return isinventoryfood(index) ? inventoryitemdefinitions[index]->foodtime : 0; }

bool getworldfurnaceconfig(int item, int &inputslots, int &inputlimit)
{
    if(inventoryitemdefinitions.inrange(item) && inventoryitemdefinitions[item]->hasfurnace)
    {
        inputslots = inventoryitemdefinitions[item]->furnaceinputslots;
        inputlimit = inventoryitemdefinitions[item]->furnaceinputlimit;
        return true;
    }
    inputslots = inputlimit = 0;
    return false;
}

bool isinventorytool(int index) { return inventoryitemdefinitions.inrange(index) && inventoryitemdefinitions[index]->hastool; }
const char *getinventorytooltype(int index) { return isinventorytool(index) ? inventoryitemdefinitions[index]->tooltype : ""; }
int getinventorytooltier(int index) { return isinventorytool(index) ? inventoryitemdefinitions[index]->tooltier : 0; }
float getinventorytoolspeed(int index) { return isinventorytool(index) ? inventoryitemdefinitions[index]->toolspeed : 1.0f; }
int getinventorytoolmaxdurability(int index) { return isinventorytool(index) ? inventoryitemdefinitions[index]->maxdurability : 0; }
int getinventorytoolcornerpush(int index) { return isinventorytool(index) ? inventoryitemdefinitions[index]->toolcornerpush : TOOL_CORNER_PUSH_NONE; }
float getinventorytooldamage(int index) { return isinventorytool(index) ? inventoryitemdefinitions[index]->tooldamage : 1.0f; }

static worlddefinition *getworlddefinition(int type, int index)
{
    if(type == WORLD_ITEM_CUBE) return worldcubedefinitions.inrange(index) ? worldcubedefinitions[index] : NULL;
    if(type == WORLD_ITEM_SCATTER || type == WORLD_ITEM_PLACEABLE)
        return worldscatterdefinitions.inrange(index) ? worldscatterdefinitions[index] : NULL;
    return NULL;
}

float getworldobjecthardness(int type, int index)
{
    worlddefinition *definition = getworlddefinition(type, index);
    return definition && definition->hasmining ? max(definition->hardness, 0.01f) : 1.0f;
}

const char *getworldobjectpreferredtool(int type, int index)
{
    worlddefinition *definition = getworlddefinition(type, index);
    return definition && definition->hasmining ? definition->preferredtool : "";
}

int getworldobjectrequiredtier(int type, int index)
{
    worlddefinition *definition = getworlddefinition(type, index);
    return definition && definition->hasmining ? definition->requiredtier : 0;
}

int getworldobjecttoolwear(int type, int index)
{
    worlddefinition *definition = getworlddefinition(type, index);
    return definition && definition->hasmining ? definition->toolwear : 1;
}

bool isworldobjecthandbreakable(int type, int index)
{
    worlddefinition *definition = getworlddefinition(type, index);
    return !definition || !definition->hasmining || definition->handbreakable;
}

const char *getinventoryitemtexture(int index) { return ""; }

float getinventoryitemworldsize(int index) { return 1.0f; }

bool getinventoryitemheldflipx(int index) { return false; }

bool getinventoryitemheldflipy(int index) { return false; }

float getinventoryitemheldsize(int index) { return 1.0f; }

int numworldcubes() { return worldcubedefinitions.length(); }

int getworldcubeitem(int index)
{
    return worldcubedefinitions.inrange(index) ? worldcubedefinitions[index]->item : -1;
}

bool isworldcubepushable(int index, int toolitem)
{
    if(!inventoryitemdefinitions.inrange(toolitem) || !inventoryitemdefinitions[toolitem]->hastool) return false;
    string tagid;
    formatstring(tagid, "pushable_with_%s", inventoryitemdefinitions[toolitem]->tooltype);
    const int tag = finditemtag(tagid);
    return tag >= 0 && itemhastag(getworldcubeitem(index), tag);
}

bool getworldchestconfig(int item, int &slots)
{
    if(inventoryitemdefinitions.inrange(item) && inventoryitemdefinitions[item]->haschest)
    {
        slots = inventoryitemdefinitions[item]->chestslots;
        return true;
    }
    slots = 0;
    return false;
}

bool getworldcubefall(int index)
{
    return worldcubedefinitions.inrange(index) && worldcubedefinitions[index]->fall;
}

int getworldcubesupportdistance(int index)
{
    return worldcubedefinitions.inrange(index) && worldcubedefinitions[index]->hassupport ? worldcubedefinitions[index]->supportdistance : 0;
}

bool getworldcubesupportdecay(int index)
{
    return worldcubedefinitions.inrange(index) && worldcubedefinitions[index]->hassupport && worldcubedefinitions[index]->supportdecay;
}

bool getworldcubesupportpersistentonplace(int index)
{
    return worldcubedefinitions.inrange(index) && worldcubedefinitions[index]->hassupport &&
           worldcubedefinitions[index]->supportpersistentonplace;
}

const char *getworldcubename(int index)
{
    return worldcubedefinitions.inrange(index) ? worldcubedefinitions[index]->id : "";
}

ullong getworldcubepersistentid(int index)
{
    return worldcubedefinitions.inrange(index) ? worldcubedefinitions[index]->persistentid
         : worldcubedefinitions.inrange(worlderrorcube) ? worldcubedefinitions[worlderrorcube]->persistentid : 0;
}

int getworldcubepersistentindex(ullong id, bool warn)
{
    int *index = worldcubepersistentindexes.access(worldpersistentkey(id));
    if(index) return *index;
    if(warn) conoutf(CON_WARN, "unknown persistent world cube ID " WORLD_ULL_FORMAT "; using error cube", id);
    return worldcubedefinitions.inrange(worlderrorcube) ? worlderrorcube : -1;
}

int numworldscatters() { return worldscatterdefinitions.length(); }

const char *getworldscattername(int index)
{
    return worldscatterdefinitions.inrange(index) ? worldscatterdefinitions[index]->id : "";
}

ullong getworldscatterpersistentid(int index)
{
    return worldscatterdefinitions.inrange(index) ? worldscatterdefinitions[index]->persistentid
         : worldscatterdefinitions.inrange(worlderrorobject) ? worldscatterdefinitions[worlderrorobject]->persistentid : 0;
}

int getworldscatterpersistentindex(ullong id, bool warn)
{
    int *index = worldscatterpersistentindexes.access(worldpersistentkey(id));
    if(index) return *index;
    if(warn) conoutf(CON_WARN, "unknown persistent world object ID " WORLD_ULL_FORMAT "; using error object", id);
    return worldscatterdefinitions.inrange(worlderrorobject) ? worlderrorobject : -1;
}

int getworlditemtype(int item)
{
    loopv(worldcubedefinitions) if(worldcubedefinitions[i]->item == item) return WORLD_ITEM_CUBE;
    loopv(worldscatterdefinitions) if(worldscatterdefinitions[i]->item == item)
        return worldscatterdefinitions[i]->placeable ? WORLD_ITEM_PLACEABLE : WORLD_ITEM_SCATTER;
    return WORLD_ITEM_NONE;
}

int getworlditemindex(int item)
{
    loopv(worldcubedefinitions) if(worldcubedefinitions[i]->item == item) return i;
    loopv(worldscatterdefinitions) if(worldscatterdefinitions[i]->item == item) return i;
    return -1;
}

float getworlditemlightradius(int item)
{
    loopv(worldscatterdefinitions) if(worldscatterdefinitions[i]->item == item) return worldscatterdefinitions[i]->lightradius;
    return 0.0f;
}

static vector<worlddropdefinition> &getstandaloneworlddrops(int type, int index)
{
    static vector<worlddropdefinition> empty;
    if(type == WORLD_ITEM_CUBE && worldcubedefinitions.inrange(index)) return worldcubedefinitions[index]->drops;
    if((type == WORLD_ITEM_SCATTER || type == WORLD_ITEM_PLACEABLE) && worldscatterdefinitions.inrange(index))
        return worldscatterdefinitions[index]->drops;
    return empty;
}

int getworldobjectdropcount(int type, int index)
{
    vector<worlddropdefinition> &drops = getstandaloneworlddrops(type, index);
    if(!drops.empty()) return drops.length();
    if(type == WORLD_ITEM_CUBE && worldcubedefinitions.inrange(index)) return worldcubedefinitions[index]->item >= 0 ? 1 : 0;
    if((type == WORLD_ITEM_SCATTER || type == WORLD_ITEM_PLACEABLE) && worldscatterdefinitions.inrange(index))
        return worldscatterdefinitions[index]->item >= 0 ? 1 : 0;
    return 0;
}

bool getworldobjectdrop(int type, int index, int dropindex, int &item, int &mincount, int &maxcount, float &chance)
{
    vector<worlddropdefinition> &drops = getstandaloneworlddrops(type, index);
    if(!drops.empty())
    {
        if(!drops.inrange(dropindex)) return false;
        const worlddropdefinition &drop = drops[dropindex];
        item = drop.item;
        mincount = drop.mincount;
        maxcount = drop.maxcount;
        chance = drop.chance;
        return item >= 0;
    }
    if(dropindex != 0) return false;
    if(type == WORLD_ITEM_CUBE && worldcubedefinitions.inrange(index)) item = worldcubedefinitions[index]->item;
    else if((type == WORLD_ITEM_SCATTER || type == WORLD_ITEM_PLACEABLE) && worldscatterdefinitions.inrange(index))
        item = worldscatterdefinitions[index]->item;
    else return false;
    mincount = maxcount = 1;
    chance = 1.0f;
    return item >= 0;
}

int getworldcubefaceslot(int index, int orient)
{
    (void)index;
    (void)orient;
    return DEFAULT_GEOM;
}

bool isworldcubesolidat(const ivec &position)
{
    (void)position;
    return false;
}

int getworldscatterindexat(const ivec &support, int orient)
{
    (void)support;
    (void)orient;
    return worlderrorobject;
}

#endif
