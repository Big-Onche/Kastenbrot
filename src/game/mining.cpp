// Data-driven mining and tool rules. World and inventory definitions expose
// stable IDs; this module owns all mining metadata associated with those IDs.

#include "game.h"

namespace
{
    struct tooldefinition
    {
        string itemid, type;
        int tier, maxdurability;
        float speed;

        tooldefinition(const char *itemid = "") : tier(0), maxdurability(0), speed(1.0f)
        {
            copystring(this->itemid, itemid);
            type[0] = '\0';
        }
    };

    struct miningdefinition
    {
        string worldid, preferredtool;
        float hardness;
        int requiredtier, toolwear;
        bool handbreakable;

        miningdefinition(const char *worldid = "") : hardness(1.0f), requiredtier(0), toolwear(1), handbreakable(true)
        {
            copystring(this->worldid, worldid);
            preferredtool[0] = '\0';
        }
    };

    static vector<tooldefinition *> tooldefinitions;
    static vector<miningdefinition *> miningdefinitions;

    static tooldefinition *findtooldefinition(const char *itemid)
    {
        loopv(tooldefinitions) if(!cubecasecmp(tooldefinitions[i]->itemid, itemid)) return tooldefinitions[i];
        return NULL;
    }

    static miningdefinition *findminingdefinition(const char *worldid)
    {
        loopv(miningdefinitions) if(!cubecasecmp(miningdefinitions[i]->worldid, worldid)) return miningdefinitions[i];
        return NULL;
    }

    static bool worlddefinitionexists(const char *worldid)
    {
        loopi(numworldcubes()) if(!cubecasecmp(getworldcubename(i), worldid)) return true;
        loopi(numworldscatters()) if(!cubecasecmp(getworldscattername(i), worldid)) return true;
        return false;
    }

    static const char *worlddefinitionid(int type, int index)
    {
        if(type == WORLD_ITEM_CUBE) return getworldcubename(index);
        if(type == WORLD_ITEM_SCATTER || type == WORLD_ITEM_PLACEABLE) return getworldscattername(index);
        return "";
    }

    static tooldefinition *gettooldefinition(int item)
    {
        return item >= 0 ? findtooldefinition(getinventoryitemid(item)) : NULL;
    }

    static miningdefinition *getminingdefinition(int type, int index)
    {
        const char *id = worlddefinitionid(type, index);
        return id[0] ? findminingdefinition(id) : NULL;
    }
}

namespace game
{
    void resetminingdefinitions()
    {
        tooldefinitions.deletecontents();
        miningdefinitions.deletecontents();
    }

    void validateminingdefinitions()
    {
        loopi(numworldcubes()) if(!findminingdefinition(getworldcubename(i)))
            conoutf(CON_WARN, "development warning: world cube %s has no worldmining data; using hardness 1, hand tier, and wear 1",
                    getworldcubename(i));
        loopi(numworldscatters()) if(!findminingdefinition(getworldscattername(i)))
            conoutf(CON_WARN, "development warning: world object %s has no worldmining data; using hardness 1, hand tier, and wear 1",
                    getworldscattername(i));
    }
}

ICOMMAND(inventorytool, "ssifi", (char *id, char *tooltype, int *tier, float *speed, int *maxdurability),
{
    if(getinventoryitemindex(id) < 0)
    {
        conoutf(CON_ERROR, "inventorytool references unknown inventory item %s", id);
        return;
    }
    if(!tooltype[0] || *tier < 0 || *speed <= 0 || *maxdurability <= 0)
    {
        conoutf(CON_ERROR, "inventorytool for %s requires a type, non-negative tier, positive speed, and positive durability", id);
        return;
    }
    tooldefinition *tool = findtooldefinition(id);
    if(!tool) tool = tooldefinitions.add(new tooldefinition(id));
    copystring(tool->type, tooltype);
    tool->tier = *tier;
    tool->speed = *speed;
    tool->maxdurability = *maxdurability;
});

ICOMMAND(worldmining, "sfsiiiN",
         (char *worldid, float *hardness, char *preferredtool, int *requiredtier, int *toolwear, int *handbreakable, int *numargs),
{
    if(!worlddefinitionexists(worldid))
    {
        conoutf(CON_ERROR, "worldmining references unknown world object %s", worldid);
        return;
    }
    if(*hardness <= 0 || *requiredtier < 0 || (*numargs >= 5 && *toolwear < 0))
    {
        conoutf(CON_ERROR, "worldmining for %s requires positive hardness and non-negative tier/wear", worldid);
        return;
    }
    miningdefinition *mining = findminingdefinition(worldid);
    if(!mining) mining = miningdefinitions.add(new miningdefinition(worldid));
    mining->hardness = *hardness;
    copystring(mining->preferredtool, preferredtool ? preferredtool : "");
    mining->requiredtier = *requiredtier;
    mining->toolwear = *numargs >= 5 ? *toolwear : 1;
    mining->handbreakable = *numargs < 6 || *handbreakable != 0;
});

bool isinventorytool(int index)
{
    return gettooldefinition(index) != NULL;
}

const char *getinventorytooltype(int index)
{
    tooldefinition *tool = gettooldefinition(index);
    return tool ? tool->type : "";
}

int getinventorytooltier(int index)
{
    tooldefinition *tool = gettooldefinition(index);
    return tool ? tool->tier : 0;
}

float getinventorytoolspeed(int index)
{
    tooldefinition *tool = gettooldefinition(index);
    return tool ? tool->speed : 1.0f;
}

int getinventorytoolmaxdurability(int index)
{
    tooldefinition *tool = gettooldefinition(index);
    return tool ? tool->maxdurability : 0;
}

float getworldobjecthardness(int type, int index)
{
    miningdefinition *mining = getminingdefinition(type, index);
    return mining ? max(mining->hardness, 0.01f) : 1.0f;
}

const char *getworldobjectpreferredtool(int type, int index)
{
    miningdefinition *mining = getminingdefinition(type, index);
    return mining ? mining->preferredtool : "";
}

int getworldobjectrequiredtier(int type, int index)
{
    miningdefinition *mining = getminingdefinition(type, index);
    return mining ? max(mining->requiredtier, 0) : 0;
}

int getworldobjecttoolwear(int type, int index)
{
    miningdefinition *mining = getminingdefinition(type, index);
    return mining ? max(mining->toolwear, 0) : 1;
}

bool isworldobjecthandbreakable(int type, int index)
{
    miningdefinition *mining = getminingdefinition(type, index);
    return !mining || mining->handbreakable;
}

bool getworlddropeligible(int type, int index, int toolitem)
{
    return getinventorytooltier(toolitem) >= getworldobjectrequiredtier(type, index);
}

int getworldbreakmillis(int type, int index, int toolitem, float quality)
{
    const bool tool = isinventorytool(toolitem);
    const char *preferred = getworldobjectpreferredtool(type, index);
    const bool correct = tool && preferred[0] && !cubecasecmp(getinventorytooltype(toolitem), preferred);
    const bool canharvest = getworlddropeligible(type, index, toolitem);
    const float speed = (tool ? getinventorytoolspeed(toolitem) * (correct ? 1.0f : 0.2f) : 1.0f) * max(quality, 0.05f);
    const float damage = speed / getworldobjecthardness(type, index) / (canharvest ? 30.0f : 100.0f);
    if(damage >= 1.0f) return 0;
    if(damage <= 1.0f / 12000.0f) return 600000;
    return int(ceilf(1.0f / damage)) * 50;
}

int getworldbreaktoolwear(int type, int index, int toolitem)
{
    if(!isinventorytool(toolitem)) return 0;
    const char *preferred = getworldobjectpreferredtool(type, index);
    const bool correct = preferred[0] && !cubecasecmp(getinventorytooltype(toolitem), preferred);
    return getworldobjecttoolwear(type, index) * (correct ? 1 : 3);
}
