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
    f.defaulth = max(4*f.pointsize/6, 1);
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

static bool stripfontsuffix(char *name, const char *suffix)
{
    int namelen = strlen(name), suffixlen = strlen(suffix);
    if(namelen < suffixlen || strcmp(name + namelen - suffixlen, suffix)) return false;
    name[namelen - suffixlen] = '\0';
    return true;
}

static font *fontvariant(font *base, char style)
{
    string root;
    copystring(root, base->name);
    bool outlined = stripfontsuffix(root, "_outline");
    if(!stripfontsuffix(root, "_bold")) stripfontsuffix(root, "_italic");

    string name;
    copystring(name, root);
    if(style == 'b') concatstring(name, "_bold");
    else if(style == 'i') concatstring(name, "_italic");
    if(outlined) concatstring(name, "_outline");
    font *variant = fonts.access(name);
    return variant ? variant : base;
}

static float draw_char(GLuint &tex, font *f, int c, float x, float y, float scale)
{
    font::charinfo &info = f->chars[c];

    if(tex != f->tex)
    {
        xtraverts += gle::end();
        tex = f->tex;
        glBindTexture(GL_TEXTURE_2D, tex);
    }

    x *= textscale;
    y *= textscale;
    scale *= textscale;

    if(info.w <= 0 || info.h <= 0) return scale*info.advance;

    float x1 = x + scale*info.offsetx,
          y1 = y + scale*info.offsety,
          x2 = x + scale*(info.offsetx + info.w),
          y2 = y + scale*(info.offsety + info.h),
          tx1 = info.x / f->texw,
          ty1 = info.y / f->texh,
          tx2 = (info.x + info.w) / f->texw,
          ty2 = (info.y + info.h) / f->texh;

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

static int text_hex_digit(int c)
{
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool text_hex_color(const char *s, bvec &color)
{
    if(!s[0] || !s[1] || !s[2]) return false;
    int r = text_hex_digit(uchar(s[0])), g = text_hex_digit(uchar(s[1])), b = text_hex_digit(uchar(s[2]));
    if(r < 0 || g < 0 || b < 0) return false;
    color = bvec(r*17, g*17, b*17);
    return true;
}

// stack[sp] is the current rendered color
static void text_color(char c, bvec *stack, int size, int &sp, int a)
{
    if(c=='s') // save color
    {
        if(sp<size-1) stack[sp+1] = stack[sp], ++sp;
    }
    else if(c=='r') // restore color
    {
        xtraverts += gle::end();
        if(sp > 0) --sp;
        gle::color(stack[sp], a);
    }
}

static void text_set_hex_color(const char *s, bvec *stack, int &sp, int a)
{
    bvec color;
    if(!text_hex_color(s, color)) return;
    if(textbright != 100) color.scale(textbright, 100);
    xtraverts += gle::end();
    stack[sp] = color;
    gle::color(color, a);
}

#define TEXTFORMAT(idx) \
    {\
        int fmt = uchar(str[idx]);\
        if(fmt == 'b' || fmt == 'i' || fmt == 'n')\
        {\
            textfont = fontvariant(basefont, fmt);\
            scale = textfont->scale/float(textfont->defaulth);\
        }\
        else if(fmt == 'c' && text_hex_color(&str[idx+1], textformatcolor))\
        {\
            TEXTCOLORHEX(idx+1)\
            idx += 3;\
        }\
        else { TEXTCOLOR(idx) }\
    }

#define TEXTSKELETON \
    font *basefont = curfont, *textfont = curfont;\
    bvec textformatcolor;\
    float lineheight = basefont->scale, y = 0, x = 0, scale = textfont->scale/float(textfont->defaulth);\
    int i;\
    for(i = 0; str[i]; i++)\
    {\
        TEXTINDEX(i)\
        int c = uchar(str[i]);\
        if(c=='\t')      { x = TEXTTAB(x); TEXTWHITE(i) }\
        else if(c==' ')  { x += scale*textfont->defaultw; TEXTWHITE(i) }\
        else if(c=='\n') { TEXTLINE(i) x = 0; y += lineheight; }\
        else if(c=='\f') { if(str[i+1]) { i++; TEXTFORMAT(i) }}\
        else if(textfont->chars.inrange(c))\
        {\
            float cw = scale*textfont->chars[c].advance;\
            if(cw <= 0) continue;\
            if(maxwidth >= 0)\
            {\
                int j = i;\
                float w = cw;\
                font *wrapfont = textfont;\
                float wrapscale = scale;\
                for(; str[i+1]; i++)\
                {\
                    int c = uchar(str[i+1]);\
                    if(c=='\f')\
                    {\
                        if(str[i+2])\
                        {\
                            int fmt = uchar(str[i+2]);\
                            if(fmt == 'b' || fmt == 'i' || fmt == 'n')\
                            {\
                                wrapfont = fontvariant(basefont, fmt);\
                                wrapscale = wrapfont->scale/float(wrapfont->defaulth);\
                            }\
                            if(fmt == 'c' && text_hex_color(&str[i+3], textformatcolor)) i += 4;\
                            else i++;\
                        }\
                        continue;\
                    }\
                    if(!wrapfont->chars.inrange(c)) break;\
                    float cw = wrapscale*wrapfont->chars[c].advance;\
                    if(cw <= 0 || w + cw > maxwidth) break;\
                    w += cw;\
                }\
                if(x + w > maxwidth && x > 0) { (void)j; TEXTLINE(j-1) x = 0; y += lineheight; }\
                TEXTWORD\
                textfont = wrapfont;\
                scale = wrapscale;\
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
                    if(c=='\f') { if(str[j+1]) { j++; TEXTFORMAT(j) }}\
                    else { float cw = scale*textfont->chars[c].advance; TEXTCHAR(j) }\
                }

#define TEXTEND(cursor) if(cursor >= i) { do { TEXTINDEX(cursor); } while(0); }

int text_visible(const char *str, float hitx, float hity, int maxwidth)
{
    #define TEXTINDEX(idx)
    #define TEXTWHITE(idx) if(y+lineheight > hity && x >= hitx) return idx;
    #define TEXTLINE(idx) if(y+lineheight > hity) return idx;
    #define TEXTCOLOR(idx)
    #define TEXTCOLORHEX(idx)
    #define TEXTCHAR(idx) x += cw; TEXTWHITE(idx)
    #define TEXTWORD TEXTWORDSKELETON
    TEXTSKELETON
    #undef TEXTINDEX
    #undef TEXTWHITE
    #undef TEXTLINE
    #undef TEXTCOLOR
    #undef TEXTCOLORHEX
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
    #define TEXTCOLORHEX(idx)
    #define TEXTCHAR(idx) x += cw;
    #define TEXTWORD TEXTWORDSKELETON if(i >= cursor) break;
    cx = cy = 0;
    TEXTSKELETON
    TEXTEND(cursor)
    #undef TEXTINDEX
    #undef TEXTWHITE
    #undef TEXTLINE
    #undef TEXTCOLOR
    #undef TEXTCOLORHEX
    #undef TEXTCHAR
    #undef TEXTWORD
}

void text_boundsf(const char *str, float &width, float &height, int maxwidth)
{
    #define TEXTINDEX(idx)
    #define TEXTWHITE(idx)
    #define TEXTLINE(idx) if(x > width) width = x;
    #define TEXTCOLOR(idx)
    #define TEXTCOLORHEX(idx)
    #define TEXTCHAR(idx) x += cw;
    #define TEXTWORD x += w;
    width = 0;
    TEXTSKELETON
    height = y + lineheight;
    TEXTLINE(_)
    #undef TEXTINDEX
    #undef TEXTWHITE
    #undef TEXTLINE
    #undef TEXTCOLOR
    #undef TEXTCOLORHEX
    #undef TEXTCHAR
    #undef TEXTWORD
}

Shader *textshader = NULL;

void draw_text(const char *str, float left, float top, int r, int g, int b, int a, int cursor, int maxwidth)
{
    #define TEXTINDEX(idx) if(idx == cursor) { cx = x; cy = y; cursorfont = textfont; cursorscale = scale; }
    #define TEXTWHITE(idx)
    #define TEXTLINE(idx)
    #define TEXTCOLOR(idx) if(usecolor) text_color(str[idx], colorstack, int(sizeof(colorstack)/sizeof(colorstack[0])), colorpos, a);
    #define TEXTCOLORHEX(idx) if(usecolor) text_set_hex_color(&str[idx], colorstack, colorpos, a);
    #define TEXTCHAR(idx) draw_char(tex, textfont, c, left+x, top+y, scale); x += cw;
    #define TEXTWORD TEXTWORDSKELETON
    bvec color(r, g, b);
    if(textbright != 100) color.scale(textbright, 100);
    bvec colorstack[10];
    colorstack[0] = color;
    int colorpos = 0;
    float cx = -FONTW, cy = 0;
    font *cursorfont = curfont;
    float cursorscale = curfont->scale/float(curfont->defaulth);
    bool usecolor = true;
    if(a < 0) { usecolor = false; a = -a; }
    (textshader ? textshader : hudtextshader)->set();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    GLuint tex = curfont->tex;
    glBindTexture(GL_TEXTURE_2D, tex);
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
        if(maxwidth >= 0 && cx >= maxwidth && cx > 0) { cx = 0; cy += curfont->scale; }
        draw_char(tex, cursorfont, '_', left+cx, top+cy, cursorscale);
        xtraverts += gle::end();
    }
    #undef TEXTINDEX
    #undef TEXTWHITE
    #undef TEXTLINE
    #undef TEXTCOLOR
    #undef TEXTCOLORHEX
    #undef TEXTCHAR
    #undef TEXTWORD
    #undef TEXTFORMAT
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
