#ifndef __GAME_MINING_H__
#define __GAME_MINING_H__

extern bool isinventorytool(int index);
extern const char *getinventorytooltype(int index);
extern int getinventorytooltier(int index);
extern float getinventorytoolspeed(int index);
extern int getinventorytoolmaxdurability(int index);
extern float getinventorytooldamage(int index);
extern float getworldobjecthardness(int type, int index);
extern const char *getworldobjectpreferredtool(int type, int index);
extern int getworldobjectrequiredtier(int type, int index);
extern int getworldobjecttoolwear(int type, int index);
extern bool isworldobjecthandbreakable(int type, int index);
extern int getworldbreakmillis(int type, int index, int toolitem, float quality = 1.0f);
extern bool getworlddropeligible(int type, int index, int toolitem);
extern int getworldbreaktoolwear(int type, int index, int toolitem);

#endif
