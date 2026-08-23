#ifndef __GAME_MINING_H__
#define __GAME_MINING_H__

extern bool isinventorytool(int index);
extern const char *getinventorytooltype(int index);
extern int getinventorytooltier(int index);
extern float getinventorytoolspeed(int index);
extern int getinventorytoolmaxdurability(int index);
extern float getinventorytooldamage(int index);
enum
{
    TOOL_QUALITY_LOW = 0,
    TOOL_QUALITY_AVERAGE,
    TOOL_QUALITY_HIGH,
    TOOL_QUALITY_COUNT
};
extern bool isqualitytool(int index);
extern float gettoolqualitymultiplier(int quality);
extern int gettoolquality(int item, int durability);
extern int gettoolqualitymaxdurability(int item, int quality);
extern int maketooldurability(int item, int quality, int remaining = -1);
extern int gettoolremainingdurability(int item, int durability);
extern bool validatetooldurability(int item, int durability);
extern int weartooldurability(int item, int durability, int wear);
extern int getcraftingtablequality(int stationitem);
extern bool parsetoolqualitysuffix(char *itemid, int &quality, bool &explicitquality);
extern float getworldobjecthardness(int type, int index);
extern const char *getworldobjectpreferredtool(int type, int index);
extern int getworldobjectrequiredtier(int type, int index);
extern int getworldobjecttoolwear(int type, int index);
extern bool isworldobjecthandbreakable(int type, int index);
extern int getworldbreakmillis(int type, int index, int toolitem, float quality = 1.0f);
extern bool getworlddropeligible(int type, int index, int toolitem);
extern int getworldbreaktoolwear(int type, int index, int toolitem);

#endif
