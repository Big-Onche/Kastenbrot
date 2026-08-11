// Mining behavior reads component data owned by the unified world definition registry.

#include "game.h"

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
