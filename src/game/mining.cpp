// Mining behavior reads component data owned by the unified world definition registry.

#include "game.h"

namespace
{
    // Average-quality tools retain the legacy plain durability representation.
    // Low/high quality use positive tagged values so existing persistence and
    // network stack formats can carry quality without inventing new world items.
    static const int QUALITY_DURABILITY_MARKER = 0x40000000;
    static const int QUALITY_DURABILITY_SHIFT = 28;
    static const int QUALITY_DURABILITY_MASK = 0x0FFFFFFF;
    static vector<char *> qualitytooltypes;

    static void registerqualitytool(const char *type)
    {
        if(!type || !type[0]) return;
        loopv(qualitytooltypes) if(!cubecasecmp(qualitytooltypes[i], type)) return;
        qualitytooltypes.add(newstring(type));
    }
}

ICOMMAND(qualitytool, "s", (char *type), registerqualitytool(type));

bool isqualitytool(int index)
{
    if(!isinventorytool(index)) return false;
    const char *type = getinventorytooltype(index);
    loopv(qualitytooltypes) if(!cubecasecmp(qualitytooltypes[i], type)) return true;
    return false;
}

float gettoolqualitymultiplier(int quality)
{
    switch(quality)
    {
        case TOOL_QUALITY_LOW: return 0.75f;
        case TOOL_QUALITY_HIGH: return 1.25f;
        default: return 1.0f;
    }
}

int gettoolquality(int item, int durability)
{
    if(!isqualitytool(item) || !(durability & QUALITY_DURABILITY_MARKER)) return TOOL_QUALITY_AVERAGE;
    const int encoded = (durability >> QUALITY_DURABILITY_SHIFT) & 0x3;
    return encoded == TOOL_QUALITY_LOW || encoded == TOOL_QUALITY_HIGH ? encoded : TOOL_QUALITY_AVERAGE;
}

int gettoolqualitymaxdurability(int item, int quality)
{
    const int base = getinventorytoolmaxdurability(item);
    if(base <= 0) return 0;
    if(!isqualitytool(item)) quality = TOOL_QUALITY_AVERAGE;
    return max(int(floorf(base * gettoolqualitymultiplier(quality) + 0.5f)), 1);
}

int maketooldurability(int item, int quality, int remaining)
{
    if(!isinventorytool(item)) return 0;
    if(!isqualitytool(item) || quality < TOOL_QUALITY_LOW || quality >= TOOL_QUALITY_COUNT) quality = TOOL_QUALITY_AVERAGE;
    const int maximum = gettoolqualitymaxdurability(item, quality);
    remaining = clamp(remaining < 0 ? maximum : remaining, 1, maximum);
    if(quality == TOOL_QUALITY_AVERAGE) return remaining;
    return QUALITY_DURABILITY_MARKER | (quality << QUALITY_DURABILITY_SHIFT) | remaining;
}

int gettoolremainingdurability(int item, int durability)
{
    if(!isinventorytool(item) || durability <= 0) return 0;
    return isqualitytool(item) && (durability & QUALITY_DURABILITY_MARKER) ? durability & QUALITY_DURABILITY_MASK : durability;
}

bool validatetooldurability(int item, int durability)
{
    if(!isinventorytool(item)) return durability == 0;
    const int quality = gettoolquality(item, durability), remaining = gettoolremainingdurability(item, durability);
    if(remaining <= 0 || remaining > gettoolqualitymaxdurability(item, quality)) return false;
    if(!(durability & QUALITY_DURABILITY_MARKER)) return quality == TOOL_QUALITY_AVERAGE;
    return isqualitytool(item) && (quality == TOOL_QUALITY_LOW || quality == TOOL_QUALITY_HIGH) &&
           (durability & ~(QUALITY_DURABILITY_MARKER | (0x3 << QUALITY_DURABILITY_SHIFT) | QUALITY_DURABILITY_MASK)) == 0;
}

int weartooldurability(int item, int durability, int wear)
{
    if(!validatetooldurability(item, durability)) return 0;
    const int remaining = max(gettoolremainingdurability(item, durability) - max(wear, 0), 0);
    return remaining > 0 ? maketooldurability(item, gettoolquality(item, durability), remaining) : 0;
}

int getcraftingtablequality(int stationitem)
{
    const int wooden = getinventoryitemindex("crafting_table"), reinforced = getinventoryitemindex("reinforced_crafting_table"),
              ultimate = getinventoryitemindex("ultimate_crafting_table");
    if(wooden >= 0 && stationitem == wooden) return TOOL_QUALITY_LOW;
    if(reinforced >= 0 && stationitem == reinforced) return TOOL_QUALITY_AVERAGE;
    if(ultimate >= 0 && stationitem == ultimate) return TOOL_QUALITY_HIGH;
    return -1;
}

bool parsetoolqualitysuffix(char *itemid, int &quality, bool &explicitquality)
{
    quality = TOOL_QUALITY_AVERAGE;
    explicitquality = false;
    if(!itemid) return false;
    char *suffix = strrchr(itemid, '#');
    if(!suffix) return true;
    explicitquality = true;
    if(!cubecasecmp(suffix, "#lq")) quality = TOOL_QUALITY_LOW;
    else if(!cubecasecmp(suffix, "#aq")) quality = TOOL_QUALITY_AVERAGE;
    else if(!cubecasecmp(suffix, "#hq")) quality = TOOL_QUALITY_HIGH;
    else return false;
    *suffix = '\0';
    return itemid[0] != '\0';
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
