#include "engine.h"

static hashnameset<font> fonts;

font *curfont = NULL;

static const int FONT_ATLAS_SIZE = 2048;

static Uint32 surfacepixel(SDL_Surface *surface, int x, int y)
{
    const uchar *p = (const uchar *)surface->pixels + y*surface->pitch + x*surface->format->BytesPerPixel;
    switch(surface->format->BytesPerPixel)
    {
        case 1: return *p;
        case 2: return *(const Uint16 *)p;
        case 3: return SDL_BYTEORDER == SDL_BIG_ENDIAN ? p[0]<<16 | p[1]<<8 | p[2] : p[0] | p[1]<<8 | p[2]<<16;
        default: return *(const Uint32 *)p;
    }
}

static void copyglyph(SDL_Surface *surface, vector<uchar> &atlas, int atlasx, int atlasy, bool inner)
{
    if(SDL_MUSTLOCK(surface) && SDL_LockSurface(surface) < 0) fatal("failed to lock SDL_ttf glyph: %s", SDL_GetError());
    loopi(surface->h) loopj(surface->w)
    {
        uchar r, g, b, a;
        SDL_GetRGBA(surfacepixel(surface, j, i), surface->format, &r, &g, &b, &a);
        uchar *dst = &atlas[((atlasy+i)*FONT_ATLAS_SIZE + atlasx+j)*4];
        if(inner) dst[0] = dst[1] = dst[2] = max(dst[0], a);
        else dst[3] = max(dst[3], a);
    }
    if(SDL_MUSTLOCK(surface)) SDL_UnlockSurface(surface);
}

static bool buildfont(font &f)
{
    if(!f.ttf) return false;

    SDL_Surface *outer[256] = { NULL }, *inner[256] = { NULL };
    f.chars.shrink(0);
    loopi(256) f.chars.add();

    TTF_SetFontOutline(f.ttf, 0);
    // Keep the established line layout while letting the glyphs occupy more
    // of it; the previous bitmap font had less empty space around each glyph.
    f.defaulth = max(5*f.pointsize/6, 1);
    int minx, maxx, miny, maxy, advance;
    f.defaultw = TTF_GlyphMetrics(f.ttf, ' ', &minx, &maxx, &miny, &maxy, &advance) ? f.defaulth/2 : max(advance, 1);

    SDL_Color white = { 255, 255, 255, 255 };
    int packx = 1, packy = 1, rowheight = 0;
    loopi(256) if(iscubeprint(i))
    {
        Uint16 glyph = Uint16(cube2uni(i));
        if(!TTF_GlyphIsProvided(f.ttf, glyph)) glyph = 0xFFFD;
        if(TTF_GlyphMetrics(f.ttf, glyph, &minx, &maxx, &miny, &maxy, &advance)) continue;

        TTF_SetFontOutline(f.ttf, f.outline);
        int outerascent = TTF_FontAscent(f.ttf);
        outer[i] = TTF_RenderGlyph_Blended(f.ttf, glyph, white);
        TTF_SetFontOutline(f.ttf, 0);
        if(!outer[i]) fatal("SDL_ttf failed rendering glyph U+%04X from %s: %s", glyph, f.file, TTF_GetError());
        inner[i] = f.outline ? TTF_RenderGlyph_Blended(f.ttf, glyph, white) : outer[i];
        if(!inner[i]) fatal("SDL_ttf failed rendering glyph U+%04X from %s: %s", glyph, f.file, TTF_GetError());

        if(packx + outer[i]->w + 1 > FONT_ATLAS_SIZE)
        {
            packx = 1;
            packy += rowheight + 1;
            rowheight = 0;
        }
        if(packy + outer[i]->h + 1 > FONT_ATLAS_SIZE) fatal("SDL_ttf font atlas is too small for %s", f.file);

        font::charinfo &info = f.chars[i];
        info.x = packx;
        info.y = packy;
        info.w = outer[i]->w;
        info.h = outer[i]->h;
        info.offsetx = min(minx, 0) - f.outline;
        info.offsety = 7*f.defaulth/8 - outerascent;
        info.advance = max(advance, 1);
        packx += outer[i]->w + 1;
        rowheight = max(rowheight, outer[i]->h);
    }

    f.texw = FONT_ATLAS_SIZE;
    f.texh = min(FONT_ATLAS_SIZE, packy + rowheight + 1);
    vector<uchar> atlas;
    atlas.pad(f.texw*f.texh*4);
    memset(atlas.getbuf(), 0, atlas.length());

    loopi(256) if(outer[i])
    {
        font::charinfo &info = f.chars[i];
        copyglyph(outer[i], atlas, int(info.x), int(info.y), false);
        copyglyph(inner[i], atlas, int(info.x)+f.outline, int(info.y)+f.outline, true);
        if(inner[i] != outer[i]) SDL_FreeSurface(inner[i]);
        SDL_FreeSurface(outer[i]);
    }

    glGenTextures(1, &f.tex);
    createtexture(f.tex, f.texw, f.texh, atlas.getbuf(), 3, 1, GL_RGBA8, GL_TEXTURE_2D, 0, 0, 0, false);
    return true;
}

void newfont(char *name, char *file, int *pointsize, int *scale, int *outline)
{
    if(!TTF_WasInit() && TTF_Init() < 0) fatal("Unable to initialize SDL_ttf: %s", TTF_GetError());

    font *f = &fonts[name];
    if(!f->name) f->name = newstring(name);
    if(f->tex) { glDeleteTextures(1, &f->tex); f->tex = 0; }
    if(f->ttf) { TTF_CloseFont(f->ttf); f->ttf = NULL; }
    DELETEA(f->file);

    f->file = newstring(file);
    f->pointsize = max(*pointsize, 1);
    f->outline = max(*outline, 0);
    f->scale = *scale > 0 ? *scale : f->pointsize;
    f->ttf = TTF_OpenFont(findfile(file, "rb"), f->pointsize);
    if(!f->ttf) fatal("Unable to load TrueType font %s: %s", file, TTF_GetError());
    if(!buildfont(*f)) fatal("Unable to build TrueType font %s", file);
}

COMMANDN(font, newfont, "ssiii");

font *findfont(const char *name)
{
    return fonts.access(name);
}

bool setfont(const char *name)
{
    font *f = fonts.access(name);
    if(!f) return false;
    curfont = f;
    return true;
}

static vector<font *> fontstack;

void pushfont()
{
    fontstack.add(curfont);
}

bool popfont()
{
    if(fontstack.empty()) return false;
    curfont = fontstack.pop();
    return true;
}

void gettextres(int &w, int &h)
{
    if(w < MINRESW || h < MINRESH)
    {
        if(MINRESW > w*MINRESH/h)
        {
            h = h*MINRESW/w;
            w = MINRESW;
        }
        else
        {
            w = w*MINRESH/h;
            h = MINRESH;
        }
    }
}

float text_widthf(const char *str)
{
    float width, height;
    text_boundsf(str, width, height);
    return width;
}

#define FONTTAB (4*FONTW)
#define TEXTTAB(x) ((int((x)/FONTTAB)+1.0f)*FONTTAB)

void tabify(const char *str, int *numtabs)
{
    int tw = max(*numtabs, 0)*FONTTAB-1, tabs = 0;
    for(float w = text_widthf(str); w <= tw; w = TEXTTAB(w)) ++tabs;
    int len = strlen(str);
    char *tstr = newstring(len + tabs);
    memcpy(tstr, str, len);
    memset(&tstr[len], '\t', tabs);
    tstr[len+tabs] = '\0';
    stringret(tstr);
}

COMMAND(tabify, "si");

void draw_textf(const char *fstr, float left, float top, ...)
{
    defvformatstring(str, top, fstr);
    draw_text(str, left, top);
}

const matrix4x3 *textmatrix = NULL;
float textscale = 1;

static float draw_char(int c, float x, float y, float scale)
{
    font::charinfo &info = curfont->chars[c];

    x *= textscale;
    y *= textscale;
    scale *= textscale;

    if(info.w <= 0 || info.h <= 0) return scale*info.advance;

    float x1 = x + scale*info.offsetx,
          y1 = y + scale*info.offsety,
          x2 = x + scale*(info.offsetx + info.w),
          y2 = y + scale*(info.offsety + info.h),
          tx1 = info.x / curfont->texw,
          ty1 = info.y / curfont->texh,
          tx2 = (info.x + info.w) / curfont->texw,
          ty2 = (info.y + info.h) / curfont->texh;

    if(textmatrix)
    {
        gle::attrib(textmatrix->transform(vec2(x1, y1))); gle::attribf(tx1, ty1);
        gle::attrib(textmatrix->transform(vec2(x2, y1))); gle::attribf(tx2, ty1);
        gle::attrib(textmatrix->transform(vec2(x2, y2))); gle::attribf(tx2, ty2);
        gle::attrib(textmatrix->transform(vec2(x1, y2))); gle::attribf(tx1, ty2);
    }
    else
    {
        gle::attribf(x1, y1); gle::attribf(tx1, ty1);
        gle::attribf(x2, y1); gle::attribf(tx2, ty1);
        gle::attribf(x2, y2); gle::attribf(tx2, ty2);
        gle::attribf(x1, y2); gle::attribf(tx1, ty2);
    }

    return scale*info.advance;
}

VARP(textbright, 0, 85, 100);

//stack[sp] is current color index
static void text_color(char c, char *stack, int size, int &sp, bvec color, int a)
{
    if(c=='s') // save color
    {
        c = stack[sp];
        if(sp<size-1) stack[++sp] = c;
    }
    else
    {
        xtraverts += gle::end();
        if(c=='r') { if(sp > 0) --sp; c = stack[sp]; } // restore color
        else stack[sp] = c;
        switch(c)
        {
            case '0': color = bvec( 64, 255, 128); break;   // green: player talk
            case '1': color = bvec( 96, 160, 255); break;   // blue: "echo" command
            case '2': color = bvec(255, 192,  64); break;   // yellow: gameplay messages
            case '3': color = bvec(255,  64,  64); break;   // red: important errors
            case '4': color = bvec(128, 128, 128); break;   // gray
            case '5': color = bvec(192,  64, 192); break;   // magenta
            case '6': color = bvec(255, 128,   0); break;   // orange
            case '7': color = bvec(255, 255, 255); break;   // white
            case '8': color = bvec( 80, 207, 229); break;   // "Tesseract Blue"
            case '9': color = bvec(160, 240, 120); break;
            default: gle::color(color, a); return;          // provided color: everything else
        }
        if(textbright != 100) color.scale(textbright, 100);
        gle::color(color, a);
    }
}

#define TEXTSKELETON \
    float y = 0, x = 0, scale = curfont->scale/float(curfont->defaulth);\
    int i;\
    for(i = 0; str[i]; i++)\
    {\
        TEXTINDEX(i)\
        int c = uchar(str[i]);\
        if(c=='\t')      { x = TEXTTAB(x); TEXTWHITE(i) }\
        else if(c==' ')  { x += scale*curfont->defaultw; TEXTWHITE(i) }\
        else if(c=='\n') { TEXTLINE(i) x = 0; y += FONTH; }\
        else if(c=='\f') { if(str[i+1]) { i++; TEXTCOLOR(i) }}\
        else if(curfont->chars.inrange(c))\
        {\
            float cw = scale*curfont->chars[c].advance;\
            if(cw <= 0) continue;\
            if(maxwidth >= 0)\
            {\
                int j = i;\
                float w = cw;\
                for(; str[i+1]; i++)\
                {\
                    int c = uchar(str[i+1]);\
                    if(c=='\f') { if(str[i+2]) i++; continue; }\
                    if(!curfont->chars.inrange(c)) break;\
                    float cw = scale*curfont->chars[c].advance;\
                    if(cw <= 0 || w + cw > maxwidth) break;\
                    w += cw;\
                }\
                if(x + w > maxwidth && x > 0) { (void)j; TEXTLINE(j-1) x = 0; y += FONTH; }\
                TEXTWORD\
            }\
            else { TEXTCHAR(i) }\
        }\
    }

//all the chars are guaranteed to be either drawable or color commands
#define TEXTWORDSKELETON \
                for(; j <= i; j++)\
                {\
                    TEXTINDEX(j)\
                    int c = uchar(str[j]);\
                    if(c=='\f') { if(str[j+1]) { j++; TEXTCOLOR(j) }}\
                    else { float cw = scale*curfont->chars[c].advance; TEXTCHAR(j) }\
                }

#define TEXTEND(cursor) if(cursor >= i) { do { TEXTINDEX(cursor); } while(0); }

int text_visible(const char *str, float hitx, float hity, int maxwidth)
{
    #define TEXTINDEX(idx)
    #define TEXTWHITE(idx) if(y+FONTH > hity && x >= hitx) return idx;
    #define TEXTLINE(idx) if(y+FONTH > hity) return idx;
    #define TEXTCOLOR(idx)
    #define TEXTCHAR(idx) x += cw; TEXTWHITE(idx)
    #define TEXTWORD TEXTWORDSKELETON
    TEXTSKELETON
    #undef TEXTINDEX
    #undef TEXTWHITE
    #undef TEXTLINE
    #undef TEXTCOLOR
    #undef TEXTCHAR
    #undef TEXTWORD
    return i;
}

//inverse of text_visible
void text_posf(const char *str, int cursor, float &cx, float &cy, int maxwidth)
{
    #define TEXTINDEX(idx) if(idx == cursor) { cx = x; cy = y; break; }
    #define TEXTWHITE(idx)
    #define TEXTLINE(idx)
    #define TEXTCOLOR(idx)
    #define TEXTCHAR(idx) x += cw;
    #define TEXTWORD TEXTWORDSKELETON if(i >= cursor) break;
    cx = cy = 0;
    TEXTSKELETON
    TEXTEND(cursor)
    #undef TEXTINDEX
    #undef TEXTWHITE
    #undef TEXTLINE
    #undef TEXTCOLOR
    #undef TEXTCHAR
    #undef TEXTWORD
}

void text_boundsf(const char *str, float &width, float &height, int maxwidth)
{
    #define TEXTINDEX(idx)
    #define TEXTWHITE(idx)
    #define TEXTLINE(idx) if(x > width) width = x;
    #define TEXTCOLOR(idx)
    #define TEXTCHAR(idx) x += cw;
    #define TEXTWORD x += w;
    width = 0;
    TEXTSKELETON
    height = y + FONTH;
    TEXTLINE(_)
    #undef TEXTINDEX
    #undef TEXTWHITE
    #undef TEXTLINE
    #undef TEXTCOLOR
    #undef TEXTCHAR
    #undef TEXTWORD
}

Shader *textshader = NULL;

void draw_text(const char *str, float left, float top, int r, int g, int b, int a, int cursor, int maxwidth)
{
    #define TEXTINDEX(idx) if(idx == cursor) { cx = x; cy = y; }
    #define TEXTWHITE(idx)
    #define TEXTLINE(idx)
    #define TEXTCOLOR(idx) if(usecolor) text_color(str[idx], colorstack, sizeof(colorstack), colorpos, color, a);
    #define TEXTCHAR(idx) draw_char(c, left+x, top+y, scale); x += cw;
    #define TEXTWORD TEXTWORDSKELETON
    char colorstack[10];
    colorstack[0] = '\0'; //indicate user color
    bvec color(r, g, b);
    if(textbright != 100) color.scale(textbright, 100);
    int colorpos = 0;
    float cx = -FONTW, cy = 0;
    bool usecolor = true;
    if(a < 0) { usecolor = false; a = -a; }
    (textshader ? textshader : hudtextshader)->set();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, curfont->tex);
    gle::color(color, a);
    gle::defvertex(textmatrix ? 3 : 2);
    gle::deftexcoord0();
    gle::begin(GL_QUADS);
    TEXTSKELETON
    TEXTEND(cursor)
    xtraverts += gle::end();
    if(cursor >= 0 && (totalmillis/250)&1)
    {
        gle::color(color, a);
        if(maxwidth >= 0 && cx >= maxwidth && cx > 0) { cx = 0; cy += FONTH; }
        draw_char('_', left+cx, top+cy, scale);
        xtraverts += gle::end();
    }
    #undef TEXTINDEX
    #undef TEXTWHITE
    #undef TEXTLINE
    #undef TEXTCOLOR
    #undef TEXTCHAR
    #undef TEXTWORD
}

void reloadfonts()
{
    enumerate(fonts, font, f, { f.tex = 0; if(!buildfont(f)) fatal("failed to reload TrueType font %s", f.file); });
}

void cleanupfonts(bool full)
{
    enumerate(fonts, font, f,
    {
        if(f.tex) { glDeleteTextures(1, &f.tex); f.tex = 0; }
        if(full && f.ttf) { TTF_CloseFont(f.ttf); f.ttf = NULL; }
    });
    if(full && TTF_WasInit()) TTF_Quit();
}
