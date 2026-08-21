#include "engine.h"
#include "worlddef.h"

#ifdef WIN32
#define WORLDDEF_ULL_FORMAT "%I64u"
#else
#define WORLDDEF_ULL_FORMAT "%llu"
#endif

static ullong worldpersistentid(const char *id)
{
    ullong hash = 14695981039346656037ULL;
    if(!id) return hash;
    for(const uchar *cursor = (const uchar *)id; *cursor; ++cursor)
    {
        const uchar c = *cursor >= 'A' && *cursor <= 'Z' ? *cursor + ('a' - 'A') : *cursor;
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

worlddefinition::worlddefinition(const char *id)
    : persistentid(worldpersistentid(id)), worldsize(1.0f), heldsize(100.0f), texsize(1), lightradius(0), hardness(1.0f), toolspeed(1.0f),
      tooldamage(2.0f),
      foodhealth(0),
      maxstack(64), item(-1), slot(DEFAULT_GEOM), sideslot(DEFAULT_GEOM), bottomslot(DEFAULT_GEOM), mapmodel(-1), furnaceinputslots(0),
      furnaceinputlimit(0), chestslots(0), foodtime(0), requiredtier(0), toolwear(1), tooltier(0), maxdurability(0),
      toolcornerpush(TOOL_CORNER_PUSH_NONE),
      supportdistance(0), gialbedo(0, 0, 0), hasitem(false),
      hasheld(false), hascube(false),
      scatter(false), placeable(false), hasmining(false), hastool(false), hasfurnace(false), haschest(false), hasfood(false), hassupport(false),
      itemstackset(false),
      scattermodelset(false),
      placeablemodelset(false), hardnessset(false), tooltierset(false), toolspeedset(false), explicitdrops(false), errorfallback(false), fall(false),
      heldflipx(false), heldflipy(false), handbreakable(true), supportdecay(false), supportpersistentonplace(false)
{
    copystring(this->id, id);
    name[0] = texture[0] = icon[0] = cubetexture[0] = sidetexture[0] = bottom[0] = bottomtexture[0] = model[0] = modelicon[0] = '\0';
    lightcolor[0] = preferredtool[0] = tooltype[0] = '\0';
}

vector<worlddefinition *> worlddefinitions;
vector<worlddefinition *> worldcubedefinitions, worldscatterdefinitions, inventoryitemdefinitions;
hashtable<worldpersistentkey, int> worldcubepersistentindexes(256), worldscatterpersistentindexes(256), inventoryitempersistentindexes(256);
int worlderrorcube = -1, worlderrorobject = -1, worlderroritem = -1;

static worlddefinition *currentworlddefinition = NULL;
enum
{
    WORLDDEF_NONE = 0, WORLDDEF_ITEM, WORLDDEF_HELD, WORLDDEF_CUBE, WORLDDEF_SCATTER, WORLDDEF_PLACEABLE, WORLDDEF_MINING, WORLDDEF_TOOL,
    WORLDDEF_FURNACE, WORLDDEF_CHEST, WORLDDEF_FOOD, WORLDDEF_SUPPORT, MATERIAL_DEFINITION, TOOL_FAMILY_DEFINITION, TOOL_OVERRIDE_DEFINITION
};
static int currentworldcomponent = WORLDDEF_NONE, worlddefinitionerrors = 0;

// Materials intentionally live outside worlddefinition. Tool generation is their first consumer, but this registry can grow properties for other
// generated content without changing the runtime item registry.
struct materialdefinition
{
    string id, name, ingredient;
    int tooltier, durability, damagebonus;
    float toolspeed;
    bool nameset, ingredientset, tooltierset, toolspeedset, durabilityset, damagebonusset;

    materialdefinition(const char *id = "")
        : tooltier(0), durability(0), damagebonus(0), toolspeed(0), nameset(false), ingredientset(false), tooltierset(false), toolspeedset(false),
          durabilityset(false), damagebonusset(false)
    {
        copystring(this->id, id);
        name[0] = ingredient[0] = '\0';
    }
};

struct toolfamilydefinition
{
    string id, name, icon, cornerpush;
    string pattern[3];
    int patternrows, itemstack;
    float itemscale, speedoffset, damagebase;
    bool nameset, iconset, patternset, itemstackset, itemscaleset, speedoffsetset, damagebaseset, usematerialdamage, mirror, held, heldflipx,
         heldflipy, cornerpushset;

    toolfamilydefinition(const char *id = "")
        : patternrows(0), itemstack(0), itemscale(0), speedoffset(0), damagebase(0), nameset(false), iconset(false), patternset(false),
          itemstackset(false), itemscaleset(false), speedoffsetset(false), damagebaseset(false), usematerialdamage(false), mirror(false), held(false),
          heldflipx(false), heldflipy(false), cornerpushset(false)
    {
        copystring(this->id, id);
        name[0] = icon[0] = cornerpush[0] = '\0';
        loopi(3) pattern[i][0] = '\0';
    }
};

struct tooloverridedefinition
{
    string id, name, icon, ingredient, cornerpush;
    int tooltier, durability;
    float toolspeed, damage;
    bool nameset, iconset, ingredientset, tooltierset, toolspeedset, durabilityset, damageset, heldset, heldflipx, heldflipy, cornerpushset, applied;

    tooloverridedefinition(const char *id = "")
        : tooltier(0), durability(0), toolspeed(0), damage(0), nameset(false), iconset(false), ingredientset(false), tooltierset(false),
          toolspeedset(false), durabilityset(false), damageset(false), heldset(false), heldflipx(false), heldflipy(false), cornerpushset(false),
          applied(false)
    {
        copystring(this->id, id);
        name[0] = icon[0] = ingredient[0] = cornerpush[0] = '\0';
    }
};

struct generatedcraftrecipe
{
    string id, output, ingredient;
    string pattern[3];
    int patternrows;
    bool mirror;

    generatedcraftrecipe() : patternrows(0), mirror(false)
    {
        id[0] = output[0] = ingredient[0] = '\0';
        loopi(3) pattern[i][0] = '\0';
    }
};

static vector<materialdefinition *> materialdefinitions;
static vector<toolfamilydefinition *> toolfamilydefinitions;
static vector<tooloverridedefinition *> tooloverridedefinitions;
static vector<generatedcraftrecipe *> generatedcraftrecipes;
static vector<char *> toolsetmaterials;
static materialdefinition *currentmaterialdefinition = NULL;
static toolfamilydefinition *currenttoolfamilydefinition = NULL;
static tooloverridedefinition *currenttooloverridedefinition = NULL;
static bool toolsetexpanded = false;

worlddefinition *findworlddefinition(const char *id)
{
    loopv(worlddefinitions) if(!cubecasecmp(worlddefinitions[i]->id, id)) return worlddefinitions[i];
    return NULL;
}

worlddefinition *findworldcube(const char *id)
{
    loopv(worldcubedefinitions) if(!cubecasecmp(worldcubedefinitions[i]->id, id)) return worldcubedefinitions[i];
    return NULL;
}

worlddefinition *findworldscatter(const char *id)
{
    loopv(worldscatterdefinitions) if(!cubecasecmp(worldscatterdefinitions[i]->id, id)) return worldscatterdefinitions[i];
    return NULL;
}

worlddefinition *findinventoryitem(const char *id)
{
    loopv(inventoryitemdefinitions) if(!cubecasecmp(inventoryitemdefinitions[i]->id, id)) return inventoryitemdefinitions[i];
    return NULL;
}

int numworlddefinitions() { return worlddefinitions.length(); }

int getworlddefinitionindex(const char *id)
{
    worlddefinition *definition = findworlddefinition(id);
    return definition ? worlddefinitions.find(definition) : -1;
}

const char *getworlddefinitionid(int index)
{
    return worlddefinitions.inrange(index) ? worlddefinitions[index]->id : "";
}

void resetworlddefinitionregistry()
{
    worlddefinitions.deletecontents();
    materialdefinitions.deletecontents();
    toolfamilydefinitions.deletecontents();
    tooloverridedefinitions.deletecontents();
    generatedcraftrecipes.deletecontents();
    toolsetmaterials.deletecontents();
    worldcubedefinitions.shrink(0);
    worldscatterdefinitions.shrink(0);
    inventoryitemdefinitions.shrink(0);
    worldcubepersistentindexes.clear();
    worldscatterpersistentindexes.clear();
    inventoryitempersistentindexes.clear();
    worlderrorcube = worlderrorobject = worlderroritem = -1;
    currentworlddefinition = NULL;
    currentmaterialdefinition = NULL;
    currenttoolfamilydefinition = NULL;
    currenttooloverridedefinition = NULL;
    currentworldcomponent = WORLDDEF_NONE;
    worlddefinitionerrors = 0;
    toolsetexpanded = false;
}

static void worlddefinitionerror(const char *message)
{
    conoutf(CON_ERROR, "worlddef \"%s\": %s", currentworlddefinition ? currentworlddefinition->id : "<none>", message);
    ++worlddefinitionerrors;
}

static void materialdefinitionerror(const char *message)
{
    conoutf(CON_ERROR, "material \"%s\": %s", currentmaterialdefinition ? currentmaterialdefinition->id : "<none>", message);
    ++worlddefinitionerrors;
}

static void toolfamilydefinitionerror(const char *message)
{
    conoutf(CON_ERROR, "toolfamily \"%s\": %s", currenttoolfamilydefinition ? currenttoolfamilydefinition->id : "<none>", message);
    ++worlddefinitionerrors;
}

static void tooloverridedefinitionerror(const char *message)
{
    conoutf(CON_ERROR, "tooloverride \"%s\": %s", currenttooloverridedefinition ? currenttooloverridedefinition->id : "<none>", message);
    ++worlddefinitionerrors;
}

static materialdefinition *findmaterialdefinition(const char *id)
{
    loopv(materialdefinitions) if(!cubecasecmp(materialdefinitions[i]->id, id)) return materialdefinitions[i];
    return NULL;
}

static toolfamilydefinition *findtoolfamilydefinition(const char *id)
{
    loopv(toolfamilydefinitions) if(!cubecasecmp(toolfamilydefinitions[i]->id, id)) return toolfamilydefinitions[i];
    return NULL;
}

static tooloverridedefinition *findtooloverridedefinition(const char *id)
{
    loopv(tooloverridedefinitions) if(!cubecasecmp(tooloverridedefinitions[i]->id, id)) return tooloverridedefinitions[i];
    return NULL;
}

static const char *worlddefinitioncommand(const char *command, int component)
{
    if(component == WORLDDEF_NONE)
    {
        if(!strcmp(command, "item")) return "worlddef_item";
        if(!strcmp(command, "held")) return "worlddef_held";
        if(!strcmp(command, "cube")) return "worlddef_cube";
        if(!strcmp(command, "scatter")) return "worlddef_scatter";
        if(!strcmp(command, "placeable")) return "worlddef_placeable";
        if(!strcmp(command, "mining")) return "worlddef_mining";
        if(!strcmp(command, "tool")) return "worlddef_tool";
        if(!strcmp(command, "furnace")) return "worlddef_furnace";
        if(!strcmp(command, "chest")) return "worlddef_chest";
        if(!strcmp(command, "food")) return "worlddef_food";
        if(!strcmp(command, "support")) return "worlddef_support";
        if(!strcmp(command, "drop")) return "worlddef_drop";
    }
    else if(component == WORLDDEF_ITEM)
    {
        if(!strcmp(command, "name")) return "worlddef_name";
        if(!strcmp(command, "stack")) return "worlddef_stack";
        if(!strcmp(command, "texture") || !strcmp(command, "model")) return "worlddef_itemtexture";
        if(!strcmp(command, "icon")) return "worlddef_icon";
        if(!strcmp(command, "scale")) return "worlddef_scale";
    }
    else if(component == WORLDDEF_HELD)
    {
        if(!strcmp(command, "flip")) return "worlddef_heldflip";
        if(!strcmp(command, "size")) return "worlddef_heldsize";
    }
    else if(component == WORLDDEF_CUBE)
    {
        if(!strcmp(command, "texture")) return "worlddef_cubetexture";
        if(!strcmp(command, "side")) return "worlddef_side";
        if(!strcmp(command, "bottom")) return "worlddef_bottom";
        if(!strcmp(command, "texsize")) return "worlddef_texsize";
        if(!strcmp(command, "falling")) return "worlddef_falling";
    }
    else if(component == WORLDDEF_SCATTER || component == WORLDDEF_PLACEABLE)
    {
        if(!strcmp(command, "model")) return "worlddef_model";
        if(component == WORLDDEF_PLACEABLE && !strcmp(command, "light")) return "worlddef_light";
        if(component == WORLDDEF_PLACEABLE && !strcmp(command, "lightcolor")) return "worlddef_lightcolor";
    }
    else if(component == WORLDDEF_MINING)
    {
        if(!strcmp(command, "hardness")) return "worlddef_hardness";
        if(!strcmp(command, "tool")) return "worlddef_miningtool";
        if(!strcmp(command, "tier")) return "worlddef_miningtier";
        if(!strcmp(command, "wear")) return "worlddef_wear";
        if(!strcmp(command, "handbreakable")) return "worlddef_handbreakable";
    }
    else if(component == WORLDDEF_TOOL)
    {
        if(!strcmp(command, "type")) return "worlddef_tooltype";
        if(!strcmp(command, "tier")) return "worlddef_tooltier";
        if(!strcmp(command, "speed")) return "worlddef_speed";
        if(!strcmp(command, "durability")) return "worlddef_durability";
        if(!strcmp(command, "damage")) return "worlddef_damage";
        if(!strcmp(command, "cornerpush")) return "worlddef_cornerpush";
    }
    else if(component == WORLDDEF_FURNACE)
    {
        if(!strcmp(command, "slots")) return "worlddef_slots";
        if(!strcmp(command, "capacity")) return "worlddef_capacity";
    }
    else if(component == WORLDDEF_CHEST)
    {
        if(!strcmp(command, "slots")) return "worlddef_chestslots";
    }
    else if(component == WORLDDEF_FOOD)
    {
        if(!strcmp(command, "health")) return "worlddef_foodhealth";
        if(!strcmp(command, "time")) return "worlddef_foodtime";
    }
    else if(component == WORLDDEF_SUPPORT)
    {
        if(!strcmp(command, "distance")) return "worlddef_supportdistance";
        if(!strcmp(command, "decay")) return "worlddef_supportdecay";
        if(!strcmp(command, "persistentonplace")) return "worlddef_supportpersistentonplace";
    }
    else if(component == MATERIAL_DEFINITION)
    {
        if(!strcmp(command, "name")) return "material_name";
        if(!strcmp(command, "ingredient")) return "material_ingredient";
        if(!strcmp(command, "tier")) return "material_tier";
        if(!strcmp(command, "speed")) return "material_speed";
        if(!strcmp(command, "durability")) return "material_durability";
        if(!strcmp(command, "damagebonus")) return "material_damagebonus";
    }
    else if(component == TOOL_FAMILY_DEFINITION)
    {
        if(!strcmp(command, "name")) return "toolfamily_name";
        if(!strcmp(command, "icon")) return "toolfamily_icon";
        if(!strcmp(command, "stack")) return "toolfamily_stack";
        if(!strcmp(command, "scale")) return "toolfamily_scale";
        if(!strcmp(command, "speedoffset")) return "toolfamily_speedoffset";
        if(!strcmp(command, "damagebase")) return "toolfamily_damagebase";
        if(!strcmp(command, "materialdamage")) return "toolfamily_materialdamage";
        if(!strcmp(command, "heldflip")) return "toolfamily_heldflip";
        if(!strcmp(command, "cornerpush")) return "toolfamily_cornerpush";
        if(!strcmp(command, "recipepattern")) return "toolfamily_recipepattern";
        if(!strcmp(command, "recipemirror")) return "toolfamily_recipemirror";
    }
    else if(component == TOOL_OVERRIDE_DEFINITION)
    {
        if(!strcmp(command, "name")) return "tooloverride_name";
        if(!strcmp(command, "icon")) return "tooloverride_icon";
        if(!strcmp(command, "ingredient")) return "tooloverride_ingredient";
        if(!strcmp(command, "tier")) return "tooloverride_tier";
        if(!strcmp(command, "speed")) return "tooloverride_speed";
        if(!strcmp(command, "durability")) return "tooloverride_durability";
        if(!strcmp(command, "damage")) return "tooloverride_damage";
        if(!strcmp(command, "heldflip")) return "tooloverride_heldflip";
        if(!strcmp(command, "cornerpush")) return "tooloverride_cornerpush";
    }
    return NULL;
}

static void definitionbodyerror(int component, const char *message)
{
    if(component == MATERIAL_DEFINITION) materialdefinitionerror(message);
    else if(component == TOOL_FAMILY_DEFINITION) toolfamilydefinitionerror(message);
    else if(component == TOOL_OVERRIDE_DEFINITION) tooloverridedefinitionerror(message);
    else worlddefinitionerror(message);
}

static void executeworlddefinitionbody(const char *body, int component)
{
    vector<char> rewritten;
    bool commandstart = true, quoted = false, comment = false;
    int depth = 0;
    for(const char *cursor = body; cursor && *cursor;)
    {
        if(comment)
        {
            const char c = *cursor++;
            rewritten.add(c);
            if(c == '\n') { comment = false; commandstart = true; }
            continue;
        }
        if(quoted)
        {
            const char c = *cursor++;
            rewritten.add(c);
            if(c == '^' && *cursor) rewritten.add(*cursor++);
            else if(c == '"') quoted = false;
            continue;
        }
        if(cursor[0] == '/' && cursor[1] == '/')
        {
            rewritten.add(*cursor++);
            rewritten.add(*cursor++);
            comment = true;
            continue;
        }
        if(*cursor == '"') { quoted = true; rewritten.add(*cursor++); continue; }
        if(*cursor == '[') { ++depth; rewritten.add(*cursor++); continue; }
        if(*cursor == ']') { if(depth > 0) --depth; rewritten.add(*cursor++); continue; }
        if(depth == 0 && (*cursor == ';' || *cursor == '\n'))
        {
            commandstart = true;
            rewritten.add(*cursor++);
            continue;
        }
        if(depth == 0 && commandstart)
        {
            if(iscubespace(*cursor)) { rewritten.add(*cursor++); continue; }
            const char *start = cursor;
            while(*cursor && !iscubespace(*cursor) && *cursor != ';' && *cursor != '[' && *cursor != ']') ++cursor;
            string command;
            copystring(command, start, min(size_t(cursor - start + 1), sizeof(command)));
            const char *replacement = worlddefinitioncommand(command, component);
            if(!replacement)
            {
                defformatstring(message, "unknown %s command \"%s\"", component == WORLDDEF_NONE ? "worlddef" : "component", command);
                definitionbodyerror(component, message);
                return;
            }
            while(*replacement) rewritten.add(*replacement++);
            commandstart = false;
            continue;
        }
        rewritten.add(*cursor++);
    }
    rewritten.add('\0');
    execute(rewritten.getbuf());
}

static bool beginworldcomponent(int component, bool &present, const char *name)
{
    if(!currentworlddefinition || currentworldcomponent != WORLDDEF_NONE)
    {
        worlddefinitionerror("component blocks must be direct children of worlddef");
        return false;
    }
    if(present)
    {
        defformatstring(message, "duplicate %s component", name);
        worlddefinitionerror(message);
        return false;
    }
    present = true;
    currentworldcomponent = component;
    return true;
}

static void endworldcomponent() { currentworldcomponent = WORLDDEF_NONE; }

static void registerworlddefinition(const char *id, const char *body)
{
    if(currentworlddefinition)
    {
        worlddefinitionerror("nested worlddef blocks are not allowed");
        return;
    }
    if(!id[0] || findworlddefinition(id))
    {
        conoutf(CON_ERROR, "duplicate or empty worlddef id \"%s\"", id);
        ++worlddefinitionerrors;
        return;
    }
    currentworlddefinition = worlddefinitions.add(new worlddefinition(id));
    executeworlddefinitionbody(body, WORLDDEF_NONE);
    currentworlddefinition = NULL;
    currentworldcomponent = WORLDDEF_NONE;
}

ICOMMAND(worlddef, "sS", (char *id, char *body),
{
    registerworlddefinition(id, body);
});

ICOMMAND(material, "sS", (char *id, char *body),
{
    if(currentmaterialdefinition || currenttoolfamilydefinition || currenttooloverridedefinition)
    {
        conoutf(CON_ERROR, "nested material, toolfamily, and tooloverride blocks are not allowed");
        ++worlddefinitionerrors;
        return;
    }
    if(!id[0] || findmaterialdefinition(id))
    {
        conoutf(CON_ERROR, "duplicate or empty material id \"%s\"", id);
        ++worlddefinitionerrors;
        return;
    }
    currentmaterialdefinition = materialdefinitions.add(new materialdefinition(id));
    executeworlddefinitionbody(body, MATERIAL_DEFINITION);
    currentmaterialdefinition = NULL;
});

ICOMMAND(toolfamily, "sS", (char *id, char *body),
{
    if(currentmaterialdefinition || currenttoolfamilydefinition || currenttooloverridedefinition)
    {
        conoutf(CON_ERROR, "nested material, toolfamily, and tooloverride blocks are not allowed");
        ++worlddefinitionerrors;
        return;
    }
    if(!id[0] || findtoolfamilydefinition(id))
    {
        conoutf(CON_ERROR, "duplicate or empty toolfamily id \"%s\"", id);
        ++worlddefinitionerrors;
        return;
    }
    currenttoolfamilydefinition = toolfamilydefinitions.add(new toolfamilydefinition(id));
    executeworlddefinitionbody(body, TOOL_FAMILY_DEFINITION);
    currenttoolfamilydefinition = NULL;
});

ICOMMAND(tooloverride, "sS", (char *id, char *body),
{
    if(currentmaterialdefinition || currenttoolfamilydefinition || currenttooloverridedefinition)
    {
        conoutf(CON_ERROR, "nested material, toolfamily, and tooloverride blocks are not allowed");
        ++worlddefinitionerrors;
        return;
    }
    if(!id[0] || findtooloverridedefinition(id))
    {
        conoutf(CON_ERROR, "duplicate or empty tooloverride id \"%s\"", id);
        ++worlddefinitionerrors;
        return;
    }
    currenttooloverridedefinition = tooloverridedefinitions.add(new tooloverridedefinition(id));
    executeworlddefinitionbody(body, TOOL_OVERRIDE_DEFINITION);
    currenttooloverridedefinition = NULL;
});

ICOMMAND(toolset, "s", (char *materials),
{
    vector<char *> selected;
    explodelist(materials, selected);
    loopv(selected) toolsetmaterials.add(newstring(selected[i]));
    selected.deletecontents();
});

ICOMMANDS("material_name", "s", (char *value),
{
    copystring(currentmaterialdefinition->name, value);
    currentmaterialdefinition->nameset = true;
});
ICOMMANDS("material_ingredient", "s", (char *value),
{
    copystring(currentmaterialdefinition->ingredient, value);
    currentmaterialdefinition->ingredientset = true;
});
ICOMMANDS("material_tier", "i", (int *value),
{
    currentmaterialdefinition->tooltier = *value;
    currentmaterialdefinition->tooltierset = true;
});
ICOMMANDS("material_speed", "f", (float *value),
{
    currentmaterialdefinition->toolspeed = *value;
    currentmaterialdefinition->toolspeedset = true;
});
ICOMMANDS("material_durability", "i", (int *value),
{
    currentmaterialdefinition->durability = *value;
    currentmaterialdefinition->durabilityset = true;
});
ICOMMANDS("material_damagebonus", "i", (int *value),
{
    currentmaterialdefinition->damagebonus = *value;
    currentmaterialdefinition->damagebonusset = true;
});

ICOMMANDS("toolfamily_name", "s", (char *value),
{
    copystring(currenttoolfamilydefinition->name, value);
    currenttoolfamilydefinition->nameset = true;
});
ICOMMANDS("toolfamily_icon", "s", (char *value),
{
    copystring(currenttoolfamilydefinition->icon, value);
    currenttoolfamilydefinition->iconset = true;
});
ICOMMANDS("toolfamily_stack", "i", (int *value),
{
    currenttoolfamilydefinition->itemstack = *value;
    currenttoolfamilydefinition->itemstackset = true;
});
ICOMMANDS("toolfamily_scale", "f", (float *value),
{
    currenttoolfamilydefinition->itemscale = *value;
    currenttoolfamilydefinition->itemscaleset = true;
});
ICOMMANDS("toolfamily_speedoffset", "f", (float *value),
{
    currenttoolfamilydefinition->speedoffset = *value;
    currenttoolfamilydefinition->speedoffsetset = true;
});
ICOMMANDS("toolfamily_damagebase", "f", (float *value),
{
    currenttoolfamilydefinition->damagebase = *value;
    currenttoolfamilydefinition->damagebaseset = true;
});
ICOMMANDS("toolfamily_materialdamage", "i", (int *value), currenttoolfamilydefinition->usematerialdamage = *value != 0);
ICOMMANDS("toolfamily_heldflip", "ii", (int *x, int *y),
{
    currenttoolfamilydefinition->held = true;
    currenttoolfamilydefinition->heldflipx = *x != 0;
    currenttoolfamilydefinition->heldflipy = *y != 0;
});
ICOMMANDS("toolfamily_cornerpush", "s", (char *value),
{
    copystring(currenttoolfamilydefinition->cornerpush, value);
    currenttoolfamilydefinition->cornerpushset = true;
});
ICOMMANDS("toolfamily_recipepattern", "s", (char *value),
{
    vector<char *> rows;
    explodelist(value, rows);
    currenttoolfamilydefinition->patternrows = min(rows.length(), 3);
    loopi(currenttoolfamilydefinition->patternrows) copystring(currenttoolfamilydefinition->pattern[i], rows[i]);
    currenttoolfamilydefinition->patternset = true;
    if(rows.length() > 3) toolfamilydefinitionerror("recipepattern cannot contain more than three rows");
    rows.deletecontents();
});
ICOMMANDS("toolfamily_recipemirror", "i", (int *value), currenttoolfamilydefinition->mirror = *value != 0);

ICOMMANDS("tooloverride_name", "s", (char *value),
{
    copystring(currenttooloverridedefinition->name, value);
    currenttooloverridedefinition->nameset = true;
});
ICOMMANDS("tooloverride_icon", "s", (char *value),
{
    copystring(currenttooloverridedefinition->icon, value);
    currenttooloverridedefinition->iconset = true;
});
ICOMMANDS("tooloverride_ingredient", "s", (char *value),
{
    copystring(currenttooloverridedefinition->ingredient, value);
    currenttooloverridedefinition->ingredientset = true;
});
ICOMMANDS("tooloverride_tier", "i", (int *value),
{
    currenttooloverridedefinition->tooltier = *value;
    currenttooloverridedefinition->tooltierset = true;
});
ICOMMANDS("tooloverride_speed", "f", (float *value),
{
    currenttooloverridedefinition->toolspeed = *value;
    currenttooloverridedefinition->toolspeedset = true;
});
ICOMMANDS("tooloverride_durability", "i", (int *value),
{
    currenttooloverridedefinition->durability = *value;
    currenttooloverridedefinition->durabilityset = true;
});
ICOMMANDS("tooloverride_damage", "f", (float *value),
{
    currenttooloverridedefinition->damage = *value;
    currenttooloverridedefinition->damageset = true;
});
ICOMMANDS("tooloverride_heldflip", "ii", (int *x, int *y),
{
    currenttooloverridedefinition->heldset = true;
    currenttooloverridedefinition->heldflipx = *x != 0;
    currenttooloverridedefinition->heldflipy = *y != 0;
});
ICOMMANDS("tooloverride_cornerpush", "s", (char *value),
{
    copystring(currenttooloverridedefinition->cornerpush, value);
    currenttooloverridedefinition->cornerpushset = true;
});

ICOMMANDS("worlddef_item", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_ITEM, currentworlddefinition->hasitem, "item")) return;
    inventoryitemdefinitions.add(currentworlddefinition);
    executeworlddefinitionbody(body, WORLDDEF_ITEM);
    endworldcomponent();
});

ICOMMANDS("worlddef_held", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_HELD, currentworlddefinition->hasheld, "held")) return;
    executeworlddefinitionbody(body, WORLDDEF_HELD);
    endworldcomponent();
});

ICOMMANDS("worlddef_cube", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_CUBE, currentworlddefinition->hascube, "cube")) return;
    worldcubedefinitions.add(currentworlddefinition);
    executeworlddefinitionbody(body, WORLDDEF_CUBE);
    endworldcomponent();
});

ICOMMANDS("worlddef_scatter", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_SCATTER, currentworlddefinition->scatter, "scatter")) return;
    if(worldscatterdefinitions.find(currentworlddefinition) < 0) worldscatterdefinitions.add(currentworlddefinition);
    executeworlddefinitionbody(body, WORLDDEF_SCATTER);
    endworldcomponent();
});

ICOMMANDS("worlddef_placeable", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_PLACEABLE, currentworlddefinition->placeable, "placeable")) return;
    if(worldscatterdefinitions.find(currentworlddefinition) < 0) worldscatterdefinitions.add(currentworlddefinition);
    executeworlddefinitionbody(body, WORLDDEF_PLACEABLE);
    endworldcomponent();
});

ICOMMANDS("worlddef_mining", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_MINING, currentworlddefinition->hasmining, "mining")) return;
    executeworlddefinitionbody(body, WORLDDEF_MINING);
    endworldcomponent();
});

ICOMMANDS("worlddef_tool", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_TOOL, currentworlddefinition->hastool, "tool")) return;
    executeworlddefinitionbody(body, WORLDDEF_TOOL);
    endworldcomponent();
});

ICOMMANDS("worlddef_furnace", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_FURNACE, currentworlddefinition->hasfurnace, "furnace")) return;
    executeworlddefinitionbody(body, WORLDDEF_FURNACE);
    endworldcomponent();
});

ICOMMANDS("worlddef_chest", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_CHEST, currentworlddefinition->haschest, "chest")) return;
    executeworlddefinitionbody(body, WORLDDEF_CHEST);
    endworldcomponent();
});

ICOMMANDS("worlddef_food", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_FOOD, currentworlddefinition->hasfood, "food")) return;
    executeworlddefinitionbody(body, WORLDDEF_FOOD);
    endworldcomponent();
});

ICOMMANDS("worlddef_support", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_SUPPORT, currentworlddefinition->hassupport, "support")) return;
    executeworlddefinitionbody(body, WORLDDEF_SUPPORT);
    endworldcomponent();
});

ICOMMANDS("worlddef_name", "s", (char *value), copystring(currentworlddefinition->name, value));
ICOMMANDS("worlddef_stack", "i", (int *value),
{
    currentworlddefinition->maxstack = *value;
    currentworlddefinition->itemstackset = true;
});
ICOMMANDS("worlddef_itemtexture", "s", (char *value), copystring(currentworlddefinition->texture, value));
ICOMMANDS("worlddef_icon", "s", (char *value), copystring(currentworlddefinition->texture, value));
ICOMMANDS("worlddef_scale", "f", (float *value), currentworlddefinition->worldsize = *value);
ICOMMANDS("worlddef_heldflip", "ii", (int *x, int *y),
{
    currentworlddefinition->heldflipx = *x != 0;
    currentworlddefinition->heldflipy = *y != 0;
});
ICOMMANDS("worlddef_heldsize", "f", (float *value), currentworlddefinition->heldsize = *value);
ICOMMANDS("worlddef_cubetexture", "s", (char *value),
{
    copystring(currentworlddefinition->cubetexture, value);
    currentworlddefinition->cubetextureset = value && value[0];
});
ICOMMANDS("worlddef_side", "s", (char *value), copystring(currentworlddefinition->sidetexture, value));
ICOMMANDS("worlddef_bottom", "s", (char *value), copystring(currentworlddefinition->bottom, value));
ICOMMANDS("worlddef_texsize", "f", (float *value), currentworlddefinition->texsize = *value);
ICOMMANDS("worlddef_falling", "i", (int *value), currentworlddefinition->fall = *value != 0);
ICOMMANDS("worlddef_model", "s", (char *value),
{
    copystring(currentworlddefinition->model, value);
    if(currentworldcomponent == WORLDDEF_SCATTER) currentworlddefinition->scattermodelset = value && value[0];
    else if(currentworldcomponent == WORLDDEF_PLACEABLE) currentworlddefinition->placeablemodelset = value && value[0];
});
ICOMMANDS("worlddef_light", "f", (float *value), currentworlddefinition->lightradius = *value);
ICOMMANDS("worlddef_lightcolor", "s", (char *value), copystring(currentworlddefinition->lightcolor, value));
ICOMMANDS("worlddef_hardness", "f", (float *value),
{
    currentworlddefinition->hardness = *value;
    currentworlddefinition->hardnessset = true;
});
ICOMMANDS("worlddef_miningtool", "s", (char *value), copystring(currentworlddefinition->preferredtool, value));
ICOMMANDS("worlddef_miningtier", "i", (int *value), currentworlddefinition->requiredtier = *value);
ICOMMANDS("worlddef_wear", "i", (int *value), currentworlddefinition->toolwear = *value);
ICOMMANDS("worlddef_handbreakable", "i", (int *value), currentworlddefinition->handbreakable = *value != 0);
ICOMMANDS("worlddef_tooltype", "s", (char *value), copystring(currentworlddefinition->tooltype, value));
ICOMMANDS("worlddef_tooltier", "i", (int *value),
{
    currentworlddefinition->tooltier = *value;
    currentworlddefinition->tooltierset = true;
});
ICOMMANDS("worlddef_speed", "f", (float *value),
{
    currentworlddefinition->toolspeed = *value;
    currentworlddefinition->toolspeedset = true;
});
ICOMMANDS("worlddef_durability", "i", (int *value), currentworlddefinition->maxdurability = *value);
ICOMMANDS("worlddef_damage", "f", (float *value), currentworlddefinition->tooldamage = *value);
ICOMMANDS("worlddef_cornerpush", "s", (char *value),
{
    if(!cubecasecmp(value, "left")) currentworlddefinition->toolcornerpush = TOOL_CORNER_PUSH_LEFT;
    else if(!cubecasecmp(value, "right")) currentworlddefinition->toolcornerpush = TOOL_CORNER_PUSH_RIGHT;
    else worlddefinitionerror("tool cornerpush must be left or right");
});
ICOMMANDS("worlddef_slots", "i", (int *value), currentworlddefinition->furnaceinputslots = *value);
ICOMMANDS("worlddef_capacity", "i", (int *value), currentworlddefinition->furnaceinputlimit = *value);
ICOMMANDS("worlddef_chestslots", "i", (int *value), currentworlddefinition->chestslots = *value);
ICOMMANDS("worlddef_foodhealth", "f", (float *value), currentworlddefinition->foodhealth = *value);
ICOMMANDS("worlddef_foodtime", "i", (int *value), currentworlddefinition->foodtime = *value);
ICOMMANDS("worlddef_supportdistance", "i", (int *value), currentworlddefinition->supportdistance = *value);
ICOMMANDS("worlddef_supportdecay", "i", (int *value), currentworlddefinition->supportdecay = *value != 0);
ICOMMANDS("worlddef_supportpersistentonplace", "i", (int *value), currentworlddefinition->supportpersistentonplace = *value != 0);

ICOMMANDS("worlddef_drop", "siifN", (char *itemid, int *mincount, int *maxcount, float *chance, int *numargs),
{
    if(!currentworlddefinition || currentworldcomponent != WORLDDEF_NONE)
    {
        worlddefinitionerror("drop must be a direct child of worlddef");
        return;
    }
    worlddropdefinition &drop = currentworlddefinition->drops.add();
    copystring(drop.itemid, itemid ? itemid : "");
    drop.item = -2;
    drop.mincount = *mincount;
    drop.maxcount = *maxcount;
    drop.chance = *numargs >= 4 ? *chance : 1.0f;
    currentworlddefinition->explicitdrops = true;
});

static bool validtooltype(const char *type)
{
    return !type[0] || !cubecasecmp(type, "pickaxe") || !cubecasecmp(type, "axe") || !cubecasecmp(type, "shovel") || !cubecasecmp(type, "sword") ||
           !cubecasecmp(type, "hammer_chisel");
}

static void toolgenerationerror(const materialdefinition *material, const toolfamilydefinition *family, const char *message)
{
    defformatstring(id, "%s_%s", material ? material->id : "?", family ? family->id : "?");
    conoutf(CON_ERROR, "failed to generate tool '%s': %s", id, message);
    ++worlddefinitionerrors;
}

static bool validcornerpush(const char *value)
{
    return value && (!cubecasecmp(value, "left") || !cubecasecmp(value, "right"));
}

static bool validfamilyrecipe(const toolfamilydefinition &family)
{
    if(!family.patternset || family.patternrows < 1 || family.patternrows > 3) return false;
    const int width = int(strlen(family.pattern[0]));
    if(width < 1 || width > 3) return false;
    bool material = false;
    loopi(family.patternrows)
    {
        if(int(strlen(family.pattern[i])) != width) return false;
        for(const char *symbol = family.pattern[i]; *symbol; ++symbol)
        {
            if(*symbol == 'M') material = true;
            else if(*symbol != 'S' && *symbol != ' ') return false;
        }
    }
    return material;
}

static bool replaceplaceholder(char *destination, size_t length, const char *source, const char *placeholder, const char *replacement)
{
    vector<char> result;
    const size_t placeholderlength = strlen(placeholder);
    for(const char *cursor = source; cursor && *cursor;)
    {
        if(!strncmp(cursor, placeholder, placeholderlength))
        {
            for(const char *value = replacement; *value; ++value) result.add(*value);
            cursor += placeholderlength;
        }
        else result.add(*cursor++);
    }
    result.add('\0');
    if(size_t(result.length()) > length) return false;
    copystring(destination, result.getbuf(), length);
    return !strchr(destination, '%');
}

static void appenddefinitiontext(vector<char> &body, const char *text)
{
    for(const char *cursor = text; cursor && *cursor; ++cursor) body.add(*cursor);
}

static void appenddefinitionescaped(vector<char> &body, const char *text)
{
    const char *escaped = escapestring(text ? text : "");
    appenddefinitiontext(body, escaped);
}

static void generateworlddeftool(const materialdefinition &material, const toolfamilydefinition &family, tooloverridedefinition *overrides,
                                 const char *id, const char *icon, const char *ingredient)
{
    const char *name = overrides && overrides->nameset ? overrides->name : NULL;
    defformatstring(defaultname, "%s %s", material.name, family.name);
    const int tier = overrides && overrides->tooltierset ? overrides->tooltier : material.tooltier;
    const int durability = overrides && overrides->durabilityset ? overrides->durability : material.durability;
    const float speed = overrides && overrides->toolspeedset ? overrides->toolspeed : material.toolspeed + family.speedoffset;
    const float damage = overrides && overrides->damageset ? overrides->damage
                                                           : family.damagebase + (family.usematerialdamage ? material.damagebonus : 0);
    const bool held = overrides && overrides->heldset ? true : family.held;
    const bool heldflipx = overrides && overrides->heldset ? overrides->heldflipx : family.heldflipx;
    const bool heldflipy = overrides && overrides->heldset ? overrides->heldflipy : family.heldflipy;
    const char *cornerpush = overrides && overrides->cornerpushset ? overrides->cornerpush : family.cornerpushset ? family.cornerpush : "";
    vector<char> body;
    appenddefinitiontext(body, "item [ name ");
    appenddefinitionescaped(body, name ? name : defaultname);
    defformatstring(itemtail, "; stack %d; icon ", family.itemstack);
    appenddefinitiontext(body, itemtail);
    appenddefinitionescaped(body, icon);
    defformatstring(scaletail, "; scale %s ]\n", floatstr(family.itemscale));
    appenddefinitiontext(body, scaletail);
    if(held)
    {
        defformatstring(heldbody, "held [ flip %d %d ]\n", heldflipx ? 1 : 0, heldflipy ? 1 : 0);
        appenddefinitiontext(body, heldbody);
    }
    appenddefinitiontext(body, "tool [ type ");
    appenddefinitionescaped(body, family.id);
    string speedtext, damagetext;
    copystring(speedtext, floatstr(speed));
    copystring(damagetext, floatstr(damage));
    defformatstring(toolbody, "; tier %d; speed %s; durability %d; damage %s", tier, speedtext, durability, damagetext);
    appenddefinitiontext(body, toolbody);
    if(cornerpush[0])
    {
        appenddefinitiontext(body, "; cornerpush ");
        appenddefinitionescaped(body, cornerpush);
    }
    appenddefinitiontext(body, " ]\n");
    body.add('\0');
    registerworlddefinition(id, body.getbuf());

    if(!findworlddefinition(id)) return;
    generatedcraftrecipe &recipe = *generatedcraftrecipes.add(new generatedcraftrecipe);
    copystring(recipe.id, id);
    copystring(recipe.output, id);
    copystring(recipe.ingredient, ingredient);
    recipe.patternrows = family.patternrows;
    loopi(recipe.patternrows) copystring(recipe.pattern[i], family.pattern[i]);
    recipe.mirror = family.mirror;
    if(overrides) overrides->applied = true;
}

static bool expandtoolsets()
{
    if(toolsetexpanded) return worlddefinitionerrors == 0;
    toolsetexpanded = true;
    if(toolsetmaterials.empty() && toolfamilydefinitions.empty()) return worlddefinitionerrors == 0;

    vector<materialdefinition *> selectedmaterials;
    loopv(toolsetmaterials)
    {
        materialdefinition *material = findmaterialdefinition(toolsetmaterials[i]);
        if(!material)
        {
            conoutf(CON_ERROR, "toolset references unknown material \"%s\"", toolsetmaterials[i]);
            ++worlddefinitionerrors;
            continue;
        }
        if(selectedmaterials.find(material) >= 0)
        {
            conoutf(CON_ERROR, "toolset contains duplicate material \"%s\"", material->id);
            ++worlddefinitionerrors;
            continue;
        }
        selectedmaterials.add(material);
        if(!material->nameset || !material->name[0] || !material->ingredientset || !material->ingredient[0] || !material->tooltierset ||
           material->tooltier < 0 || !material->toolspeedset || material->toolspeed <= 0 || !material->durabilityset || material->durability <= 0 ||
           !material->damagebonusset)
        {
            conoutf(CON_ERROR,
                    "material \"%s\": selected tool material requires name, ingredient, non-negative tier, "
                    "positive speed/durability, and damagebonus",
                    material->id);
            ++worlddefinitionerrors;
        }
    }
    loopv(toolfamilydefinitions)
    {
        toolfamilydefinition &family = *toolfamilydefinitions[i];
        if(!family.nameset || !family.name[0] || !family.iconset || !family.icon[0] || !family.itemstackset || family.itemstack <= 0 ||
           !family.itemscaleset || family.itemscale <= 0 || !family.damagebaseset || family.damagebase <= 0 || !validtooltype(family.id))
        {
            conoutf(CON_ERROR, "toolfamily \"%s\": requires name, icon, positive stack/scale/damagebase, and a valid tool type", family.id);
            ++worlddefinitionerrors;
        }
        if(!validfamilyrecipe(family))
        {
            conoutf(CON_ERROR, "toolfamily \"%s\": malformed recipe pattern (expected equal 1-3 character rows using M, S, and spaces)", family.id);
            ++worlddefinitionerrors;
        }
        if(family.cornerpushset && !validcornerpush(family.cornerpush))
        {
            conoutf(CON_ERROR, "toolfamily \"%s\": cornerpush must be left or right", family.id);
            ++worlddefinitionerrors;
        }
    }

    vector<char *> generatedids;
    loopv(selectedmaterials) loopvj(toolfamilydefinitions)
    {
        materialdefinition &material = *selectedmaterials[i];
        toolfamilydefinition &family = *toolfamilydefinitions[j];
        string id;
        const int idlength = snprintf(id, sizeof(id), "%s_%s", material.id, family.id);
        if(idlength < 1 || idlength >= int(sizeof(id)))
        {
            toolgenerationerror(&material, &family, "generated id is too long");
            continue;
        }
        bool duplicate = findworlddefinition(id) != NULL;
        loopvk(generatedids) if(!cubecasecmp(generatedids[k], id)) { duplicate = true; break; }
        if(duplicate)
        {
            toolgenerationerror(&material, &family, "duplicate generated worlddef id");
            continue;
        }
        generatedids.add(newstring(id));
        string icon;
        tooloverridedefinition *overrides = findtooloverridedefinition(id);
        const char *iconsource = overrides && overrides->iconset ? overrides->icon : family.icon;
        if(!replaceplaceholder(icon, sizeof(icon), iconsource, "%material%", material.id))
            toolgenerationerror(&material, &family, "icon contains an unresolved placeholder or is too long");
        if(overrides && overrides->cornerpushset && !validcornerpush(overrides->cornerpush))
            toolgenerationerror(&material, &family, "override cornerpush must be left or right");
        const char *ingredient = overrides && overrides->ingredientset ? overrides->ingredient : material.ingredient;
        if(!ingredient[0]) toolgenerationerror(&material, &family, "material has no crafting ingredient");
    }
    generatedids.deletecontents();
    if(worlddefinitionerrors) return false;

    loopv(selectedmaterials) loopvj(toolfamilydefinitions)
    {
        materialdefinition &material = *selectedmaterials[i];
        toolfamilydefinition &family = *toolfamilydefinitions[j];
        defformatstring(id, "%s_%s", material.id, family.id);
        string icon;
        tooloverridedefinition *overrides = findtooloverridedefinition(id);
        replaceplaceholder(icon, sizeof(icon), overrides && overrides->iconset ? overrides->icon : family.icon, "%material%", material.id);
        const char *ingredient = overrides && overrides->ingredientset ? overrides->ingredient : material.ingredient;
        generateworlddeftool(material, family, overrides, id, icon, ingredient);
    }
    loopv(tooloverridedefinitions) if(!tooloverridedefinitions[i]->applied)
    {
        conoutf(CON_ERROR, "tooloverride \"%s\": does not name a generated tool", tooloverridedefinitions[i]->id);
        ++worlddefinitionerrors;
    }
    return worlddefinitionerrors == 0;
}

int numgeneratedcraftrecipes() { return generatedcraftrecipes.length(); }
const char *getgeneratedcraftrecipeid(int recipe) { return generatedcraftrecipes.inrange(recipe) ? generatedcraftrecipes[recipe]->id : ""; }
const char *getgeneratedcraftrecipeoutput(int recipe) { return generatedcraftrecipes.inrange(recipe) ? generatedcraftrecipes[recipe]->output : ""; }
const char *getgeneratedcraftrecipeingredient(int recipe)
{
    return generatedcraftrecipes.inrange(recipe) ? generatedcraftrecipes[recipe]->ingredient : "";
}
int getgeneratedcraftrecipepatternrows(int recipe) { return generatedcraftrecipes.inrange(recipe) ? generatedcraftrecipes[recipe]->patternrows : 0; }
const char *getgeneratedcraftrecipepatternrow(int recipe, int row)
{
    return generatedcraftrecipes.inrange(recipe) && row >= 0 && row < generatedcraftrecipes[recipe]->patternrows
         ? generatedcraftrecipes[recipe]->pattern[row] : "";
}
bool getgeneratedcraftrecipemirror(int recipe) { return generatedcraftrecipes.inrange(recipe) && generatedcraftrecipes[recipe]->mirror; }

static bool buildworldpersistentindexes()
{
    bool valid = true;
    worldcubepersistentindexes.clear();
    worldscatterpersistentindexes.clear();
    inventoryitempersistentindexes.clear();
    loopk(3)
    {
        vector<worlddefinition *> &definitions = k == 0 ? worldcubedefinitions : k == 1 ? worldscatterdefinitions : inventoryitemdefinitions;
        hashtable<worldpersistentkey, int> &indexes = k == 0 ? worldcubepersistentindexes
                                                        : k == 1 ? worldscatterpersistentindexes : inventoryitempersistentindexes;
        loopv(definitions)
        {
            worlddefinition &definition = *definitions[i];
            int *previous = indexes.access(worldpersistentkey(definition.persistentid));
            if(previous && cubecasecmp(definitions[*previous]->id, definition.id))
            {
                conoutf(CON_ERROR, "persistent world definition ID collision " WORLDDEF_ULL_FORMAT " between \"%s\" and \"%s\"",
                        definition.persistentid, definitions[*previous]->id, definition.id);
                valid = false;
            }
            else indexes.access(worldpersistentkey(definition.persistentid), i);
        }
    }
    return valid;
}

bool resolveworlddefinitionregistry()
{
    if(!expandtoolsets()) return false;
    loopv(worlddefinitions)
    {
        worlddefinition &definition = *worlddefinitions[i];
        definition.item = definition.hasitem ? inventoryitemdefinitions.find(&definition) : -1;
        if(definition.hasitem && (!definition.name[0] || !definition.itemstackset || definition.maxstack <= 0))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": item requires name and a positive stack", definition.id);
            ++worlddefinitionerrors;
        }
        if(definition.hasheld && (!definition.hasitem || definition.heldsize <= 0))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": held requires item and a positive size percentage", definition.id);
            ++worlddefinitionerrors;
        }
        if(definition.hascube && (!definition.cubetextureset || !definition.cubetexture[0] || definition.texsize <= 0))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": cube requires texture and a positive texsize", definition.id);
            ++worlddefinitionerrors;
        }
        if((definition.scatter && (!definition.scattermodelset || !definition.model[0])) ||
           (definition.placeable && (!definition.placeablemodelset || !definition.model[0])))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": scatter/placeable requires model", definition.id);
            ++worlddefinitionerrors;
        }
        if(definition.hasmining && (!definition.hardnessset || definition.hardness <= 0 || definition.requiredtier < 0 || definition.toolwear < 0))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": mining requires positive hardness and non-negative tier/wear", definition.id);
            ++worlddefinitionerrors;
        }
        if(!validtooltype(definition.preferredtool))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": unknown mining tool type \"%s\"", definition.id, definition.preferredtool);
            ++worlddefinitionerrors;
        }
        if(definition.hastool && (!definition.hasitem || !definition.tooltype[0] || !definition.tooltierset || definition.tooltier < 0 ||
           !definition.toolspeedset || definition.toolspeed <= 0 || definition.maxdurability <= 0 || definition.tooldamage <= 0))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": tool requires item, type, non-negative tier, positive speed, durability, and damage", definition.id);
            ++worlddefinitionerrors;
        }
        if(definition.hastool && !validtooltype(definition.tooltype))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": unknown tool type \"%s\"", definition.id, definition.tooltype);
            ++worlddefinitionerrors;
        }
        if(definition.hasfurnace && (!definition.hascube || definition.furnaceinputslots < 1 || definition.furnaceinputslots > FURNACE_INPUT_MAX ||
           definition.furnaceinputlimit < 1))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": furnace requires cube, 1-%d slots, and positive capacity", definition.id, FURNACE_INPUT_MAX);
            ++worlddefinitionerrors;
        }
        if(definition.haschest && (!definition.hasitem || !definition.placeable || definition.chestslots < 1 ||
           definition.chestslots > CHEST_SLOTS_MAX))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": chest requires a placeable item and 1-%d slots", definition.id, CHEST_SLOTS_MAX);
            ++worlddefinitionerrors;
        }
        if(definition.hasfood && (!definition.hasitem || definition.foodhealth <= 0 || definition.foodtime <= 0))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": food requires item, positive health, and positive time", definition.id);
            ++worlddefinitionerrors;
        }
        if(definition.hassupport && (!definition.hascube || definition.supportdistance <= 0))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": support requires cube and a positive distance", definition.id);
            ++worlddefinitionerrors;
        }
        loopv(definition.drops)
        {
            worlddropdefinition &drop = definition.drops[i];
            if(!drop.itemid[0] || !cubecasecmp(drop.itemid, "false"))
            {
                if(drop.mincount < 0 || drop.maxcount < drop.mincount || drop.chance < 0 || drop.chance > 1)
                {
                    conoutf(CON_ERROR, "worlddef \"%s\": invalid drop quantity", definition.id);
                    ++worlddefinitionerrors;
                }
                drop.item = -1;
                continue;
            }
            worlddefinition *target = !cubecasecmp(drop.itemid, "self") ? &definition : findworlddefinition(drop.itemid);
            if(!target || !target->hasitem || drop.mincount < 0 || drop.maxcount < drop.mincount || drop.chance < 0 || drop.chance > 1)
            {
                conoutf(CON_ERROR, "worlddef \"%s\": invalid drop target or quantity for \"%s\"", definition.id, drop.itemid);
                ++worlddefinitionerrors;
                continue;
            }
            drop.item = inventoryitemdefinitions.find(target);
        }
    }
    if(worlddefinitionerrors || !buildworldpersistentindexes()) return false;
    worlddefinition *error = findworlddefinition("error");
    if(!error || !error->hasitem || !error->hascube || !error->scatter || !error->placeable)
    {
        conoutf(CON_ERROR, "worlddef \"error\" must contain item, cube, scatter, and placeable components");
        return false;
    }
    worlderroritem = inventoryitemdefinitions.find(error);
    worlderrorcube = worldcubedefinitions.find(error);
    worlderrorobject = worldscatterdefinitions.find(error);
    return true;
}
