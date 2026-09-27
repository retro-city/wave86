/*
 * ui.c - the synthwave text mode front end.
 * 80x25, direct video memory, CP437 box drawing, sunset gradients.
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <i86.h>
#include "wave86.h"

static FILE *thumb_open(const Game *g, int *cols, int *rows);
static void thumb_draw(FILE *f, int cols, int rows);

#define A(fg, bg) (unsigned char)(((bg) << 4) | (fg))

/* CP437 */
#define CH_BLOCK   219
#define CH_HALF_LO 220
#define CH_HALF_HI 223
#define CH_SHADE1  176
#define CH_SHADE2  177
#define CH_SHADE3  178
#define CH_H       196
#define CH_V       179
#define CH_TL      218
#define CH_TR      191
#define CH_BL      192
#define CH_BR      217
#define CH_ARROW   16
#define CH_UP      24
#define CH_DOWN    25
#define CH_DOT     250

#define LIST_X     1
#define LIST_W     37
#define PANE_X     39
#define PANE_W     40
#define PANE_TOP   7
#define PANE_H     16          /* rows 7..22 */
#define LIST_ROWS  14          /* rows 8..21 */
#define QBOX_Y     5           /* the queue box: rows 5..20 at most */
#define QBOX_ROWS  12
#define MENU_Y     13          /* the menu: the bottom half, rows 13..24 */
#define MENU_ROWS  8           /* items per column, two columns */
#define MENU_COLW  39

static void draw_logo(void)
{
    int r, i;
    /* a cell is '#' in the row's colour, or a hex digit naming its own */
    if (theme->shadow) {                /* one cell down and right, under the blocks */
        for (r = 0; r < 5; r++) {
            const char *s = theme->logo[r];
            for (i = 0; s[i]; i++)
                if (s[i] != ' ')
                    scr_put(3 + i, 2 + r, CH_SHADE1, A(theme->shadow, 0));
        }
    }
    for (r = 0; r < 5; r++) {
        const char *s = theme->logo[r];
        for (i = 0; s[i]; i++) {
            unsigned char c = (unsigned char)s[i];
            if (c == ' ') continue;
            if (c != '#')
                c = (unsigned char)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
            else
                c = theme->logo_clr[r];
            scr_put(2 + i, 1 + r, CH_BLOCK, A(c, 0));
        }
    }
    scr_puts(50, 1, theme->tagline, A(11, 0));
    scr_puts(50, 2, cpu_desc[0] ? cpu_desc : "FOR 8086 AND UP", A(8, 0));
}

static void draw_divider(void)
{
    /* sunset gradient band, left to right */
    int x;
    for (x = 0; x < 80; x++)
        scr_put(x, 6, CH_HALF_HI, A(theme->band[x * 6 / 80], 0));
}

static void draw_box(int x, int y, int w, int h, unsigned char attr)
{
    int i;
    scr_put(x, y, CH_TL, attr);
    scr_put(x + w - 1, y, CH_TR, attr);
    scr_put(x, y + h - 1, CH_BL, attr);
    scr_put(x + w - 1, y + h - 1, CH_BR, attr);
    scr_hline(x + 1, y, w - 2, CH_H, attr);
    scr_hline(x + 1, y + h - 1, w - 2, CH_H, attr);
    for (i = 1; i < h - 1; i++) {
        scr_put(x, y + i, CH_V, attr);
        scr_put(x + w - 1, y + i, CH_V, attr);
    }
}

/* a rounded tag: half-block ends in the colour, the text inverse on it */
static void pill(int *x, int y, const char *text, unsigned char clr)
{
    int n = (int)strlen(text);
    if (*x + n + 3 > PANE_X + PANE_W - 1)
        return;
    scr_put(*x, y, 222, A(clr, 0));            /* right half block */
    scr_puts(*x + 1, y, text, A(0, clr));
    scr_put(*x + 1 + n, y, 221, A(clr, 0));    /* left half block */
    *x += n + 3;
}

static void keychip_at(int *x, int y, const char *key, const char *label, int dim)
{
    scr_puts(*x, y, key, dim ? A(8, 0) : A(0, 3));
    *x += strlen(key);
    scr_put(*x, y, ' ', A(7, 0));
    *x += 1;
    scr_puts(*x, y, label, dim ? A(8, 0) : A(7, 0));
    *x += strlen(label) + 2;
}

static void keychip(int *x, const char *key, const char *label, int dim)
{
    keychip_at(x, 23, key, label, dim);
}

void ui_keybar(const Game *sel)
{
    int x = 2;
    char updown[4];

    scr_fill(0, 23, 80, 1, ' ', A(7, 0));
    updown[0] = CH_UP; updown[1] = CH_DOWN; updown[2] = 0;
    (void)updown;
    keychip(&x, "ENTER", sel && (sel->flags & GF_NETPEND) ? "INSTALL" : "RUN", game_count == 0);
    keychip(&x, "S", "SETUP", !sel || !sel->setup[0] || (sel->flags & GF_NETPEND));
    keychip(&x, "N", "NET", 0);                  /* with no server= it asks; R (rescan) still works */
    keychip(&x, "F2", "NAME", game_count == 0);
    keychip(&x, "/", "SEARCH", game_count == 0);
    keychip(&x, "DEL", "REMOVE", game_count == 0);
    keychip(&x, "M", "MENU", 0);        /* everything, with its key */
    keychip(&x, "ESC", "QUIT", 0);
}

/* header, under the tagline: current tune + volume bar */
#define MUS_X 50
#define MUS_Y 4

#define BAR_X (MUS_X + 2 + 8 + 2)   /* glyph, gap, 8-char name, gap */
#define BAR_W 10
#define VOL_SHOW_TICKS 27           /* ~1.5 s at 18.2 Hz */

static unsigned long vol_show_until = 0;
static int vu_shown = -1;

static unsigned long bios_ticks(void)
{
    return *(unsigned long __far *)MK_FP(0x40, 0x6C);
}

/* the 10-cell bar: VU in cyan/pink/gold, or the volume in gold */
static void draw_bar(int cells, int volume_mode)
{
    int i;
    for (i = 0; i < BAR_W; i++) {
        unsigned char lit = volume_mode ? A(14, 0) :
                            (i < 6 ? A(11, 0) : (i < 8 ? A(13, 0) : A(14, 0)));
        scr_put(BAR_X + i, MUS_Y, i < cells ? 254 : CH_DOT,
                i < cells ? lit : A(8, 0));
    }
}

static void music_status(void)
{
    char buf[32];

    scr_fill(MUS_X, MUS_Y, 80 - MUS_X, 1, ' ', A(7, 0));
    if (!mus_present) {
        scr_puts(MUS_X, MUS_Y, "NO SOUND CARD", A(8, 0));
        return;
    }
    if (!mus_ntracks) {
        scr_puts(MUS_X, MUS_Y, "NO MUSIC", A(8, 0));
        return;
    }
    if (vol_show_until && bios_ticks() < vol_show_until) {
        /* adjusting: the VU turns into the volume bar for a moment */
        scr_put(MUS_X, MUS_Y, mus_kind ? 14 : 13, mus_on ? A(13, 0) : A(8, 0));
        scr_puts(MUS_X + 2, MUS_Y, "VOLUME", A(14, 0));
        draw_bar(mus_vol, 1);
        return;
    }
    if (!mus_on) {
        sprintf(buf, "%c MUSIC OFF", 14);
        scr_puts(MUS_X, MUS_Y, buf, A(8, 0));
        return;
    }
    scr_put(MUS_X, MUS_Y, mus_kind ? 14 : 13, A(13, 0));   /* MOD: double note */
    scr_puts(MUS_X + 2, MUS_Y, mus_track, A(11, 0));
    vu_shown = mus_vu();
    draw_bar(vu_shown, 0);
    sprintf(buf, "VOL %d", mus_vol);        /* where + and - have it */
    scr_puts(80 - 7, MUS_Y, buf, A(8, 0));
}

/* +/- pressed: show the volume bar instead of the VU for a while */
void ui_music_volshow(void)
{
    vol_show_until = bios_ticks() + VOL_SHOW_TICKS;
    music_status();
}

/* idle hook, once per BIOS tick: animate the VU, expire the volume bar */
void ui_music_tick(void)
{
    static unsigned long last = 0;
    unsigned long t = bios_ticks();
    int cells;

    if (t == last)
        return;
    last = t;
    if (vol_show_until) {
        if (t < vol_show_until)
            return;
        vol_show_until = 0;
        music_status();             /* back to the track name and VU */
        return;
    }
    if (!mus_present || !mus_ntracks || !mus_on)
        return;
    cells = mus_vu();
    if (cells != vu_shown) {
        vu_shown = cells;
        draw_bar(cells, 0);
    }
}

void ui_status(const char *msg)
{
    char buf[81];
    scr_fill(0, 24, 80, 1, ' ', A(8, 0));
    if (msg) {
        scr_puts(2, 24, msg, A(12, 0));
    } else {
        sprintf(buf, "%d GAMES %c %s %c %uK FREE",
                game_count, CH_DOT,
                vid_is_vga ? "VGA" : (vid_is_color ? "CGA/EGA" : "MONO"),
                CH_DOT, dos_free_kb());
        scr_puts(2, 24, buf, A(8, 0));
    }
    scr_puts(80 - 2 - (int)(sizeof("V" VERSION_STR) - 1), 24, "V" VERSION_STR, A(8, 0));   /* right-aligned, whatever its length */
    music_status();
}

void ui_static(void)
{
    char t[16];

    scr_fill(0, 0, 80, 25, ' ', A(7, 0));
    draw_logo();
    draw_divider();

    draw_box(LIST_X, PANE_TOP, LIST_W, PANE_H, A(5, 0));
    sprintf(t, " GAMES %d ", game_count);
    scr_puts(LIST_X + 2, PANE_TOP, t, A(13, 0));

    draw_box(PANE_X, PANE_TOP, PANE_W, PANE_H, A(5, 0));
    scr_puts(PANE_X + 2, PANE_TOP, " DETAILS ", A(13, 0));
}

void ui_list(int sel, int top)
{
    int i;

    for (i = 0; i < LIST_ROWS; i++) {
        int gi = top + i;
        int y = 8 + i;
        scr_fill(LIST_X + 1, y, LIST_W - 2, 1, ' ', A(7, 0));
        if (gi >= game_count)
            continue;
        {
            const Game *g = &games[gi];
            int is_sel = (gi == sel);
            unsigned char at = is_sel ? A(15, 5) : (g->flags & GF_NETPEND) ? A(8, 0) : A(7, 0);
            char nm[LIST_W];

            if (is_sel) {
                scr_fill(LIST_X + 1, y, LIST_W - 2, 1, ' ', at);
                scr_put(LIST_X + 1, y, CH_ARROW, A(14, 5));
            }
            strncpy(nm, g->name, LIST_W - 4);   /* the whole row is the name */
            nm[LIST_W - 4] = 0;
            scr_puts(LIST_X + 3, y, nm, at);
        }
    }

    /* scroll marks */
    scr_put(LIST_X + LIST_W - 1, 8, top > 0 ? CH_UP : CH_V, A(13, 0));
    scr_put(LIST_X + LIST_W - 1, 8 + LIST_ROWS - 1,
            top + LIST_ROWS < game_count ? CH_DOWN : CH_V, A(13, 0));

    if (game_count == 0) {
        scr_puts(LIST_X + 4, 12, "NO GAMES FOUND", A(12, 0));
        scr_puts(LIST_X + 4, 14, "PUT EACH GAME IN ITS OWN", A(7, 0));
        scr_puts(LIST_X + 4, 15, "FOLDER UNDER:", A(7, 0));
        scr_puts(LIST_X + 4, 16, gamedir, A(11, 0));
    }
}

static void field(int y, const char *label, const char *val,
                  unsigned char vattr)
{
    scr_puts(PANE_X + 2, y, label, A(3, 0));
    scr_puts(PANE_X + 9, y, val, vattr);
}

int ui_show_details = 0;        /* P: the details instead of the picture */

void ui_details(int sel)
{
    int y, cols, rows;
    char buf[PATH_LEN + FN_LEN + 2];
    const Game *g;
    FILE *thumb;

    for (y = PANE_TOP + 1; y < PANE_TOP + PANE_H - 1; y++)
        scr_fill(PANE_X + 1, y, PANE_W - 2, 1, ' ', A(7, 0));

    if (game_count == 0) {
        scr_puts(PANE_X + 2, 9, "NOTHING TO SHOW YET.", A(8, 0));
        return;
    }
    g = &games[sel];

    strncpy(buf, g->name, PANE_W - 4);
    buf[PANE_W - 4] = 0;
    scr_puts(PANE_X + 2, 9, buf, A(15, 0));
    scr_hline(PANE_X + 2, 10, strlen(buf), CH_H, A(5, 0));

    /* a game with a picture shows it under its name and nothing else;
       P brings the details (and the tags) instead */
    thumb = ui_show_details ? NULL : thumb_open(g, &cols, &rows);
    if (thumb) {
        thumb_draw(thumb, cols, rows);
        if (g->flags & GF_NETPEND)      /* between the name and the picture */
            scr_puts(PANE_X + 2, 11, "PENDING: ENTER FETCHES THE REST.", A(12, 0));
        return;
    }

    sprintf(buf, "%s\\%s", gamedir, g->dir);
    if (strlen(buf) > PANE_W - 11)
        buf[PANE_W - 11] = 0;
    field(11, "PATH", buf, A(7, 0));
    {
        /* EXEC with the sound mode beside it, SETUP with the arguments */
        const char *m = g->sound[0] ? g->sound : ini_global("sound");
        int x = PANE_X + 9 + (int)strlen(g->exe) + 2;
        field(12, "EXEC", g->exe, A(7, 0));
        if (m && m[0] && x + 6 + (int)strlen(m) < PANE_X + PANE_W - 1) {
            char up[16];
            int k;
            for (k = 0; m[k] && k < 15; k++)
                up[k] = (char)toupper((unsigned char)m[k]);
            up[k] = 0;
            scr_puts(x, 12, "SOUND", A(3, 0));
            scr_puts(x + 6, 12, up, A(7, 0));
        }
        field(13, "SETUP", g->setup[0] ? g->setup : "none",
              g->setup[0] ? A(7, 0) : A(8, 0));
        x = PANE_X + 9 + (int)strlen(g->setup[0] ? g->setup : "none") + 2;
        if (g->args[0] && x + 5 + (int)strlen(g->args) < PANE_X + PANE_W - 1) {
            scr_puts(x, 13, "ARGS", A(3, 0));
            scr_puts(x + 5, 13, g->args, A(7, 0));
        }
    }
    {
        /* row 14: the tags as pills */
        int x = PANE_X + 2;
        if (g->flags & GF_DOS4GW)  pill(&x, 14, "386+", 12);
        if (g->flags & GF_EXODOS)  pill(&x, 14, "eXoDOS", 11);
        if (g->flags & GF_TDC)     pill(&x, 14, "TDC", 11);
        if (g->cdimg[0] || ((g->flags & GF_CDBAT) && !(g->flags & GF_NETCD)))
            pill(&x, 14, "CD", 10);     /* the disc is on this machine */
        else if (g->flags & GF_CDBAT)
            pill(&x, 14, "NET CD", 10); /* it comes off the server as it plays */
        if (g->flags & GF_NETPEND)
            pill(&x, 14, "PENDING", 12); /* the install off the server stopped part way */
    }

    if (g->flags & GF_NETPEND) {
        scr_puts(PANE_X + 2, 20, "NOT ALL OF IT IS HERE YET.", A(12, 0));
        scr_puts(PANE_X + 2, 21, "ENTER FETCHES THE REST.", A(8, 0));
    } else {
        scr_puts(PANE_X + 2, 20, "PRESS ENTER TO RUN THE GAME.", A(8, 0));
        scr_puts(PANE_X + 2, 21, "THE MENU RETURNS WHEN IT ENDS.", A(8, 0));
    }
}

/* F2: the name being typed, white on the selection colour, gold cursor */
/* the bottom line as a question: "SEARCH: keen_" */
void ui_prompt(const char *label, const char *text)
{
    int x = 2 + strlen(label) + 1;
    scr_fill(0, 24, 80, 1, ' ', A(8, 0));
    scr_puts(2, 24, label, A(12, 0));
    scr_puts(x, 24, text, A(15, 0));
    scr_put(x + strlen(text), 24, CH_BLOCK, A(14, 0));
}

void ui_edit_field(const char *text)
{
    int w = PANE_W - 4;
    unsigned len = strlen(text);
    scr_fill(PANE_X + 2, 9, w, 1, ' ', A(15, 5));
    scr_puts(PANE_X + 2, 9, text, A(15, 5));
    if ((int)len < w)
        scr_put(PANE_X + 2 + len, 9, CH_BLOCK, A(14, 5));
}



/*
 * The demoscene thumbnail: THUMBS\<DIR>.THM is a 26x7 cell picture
 * shown centred under the details. Cells are custom glyphs loaded into
 * the two VGA font banks (512-character mode), two colours each; see
 * tools/makethumb.py. Text mode, no pixels. Returns 1 when drawn.
 */
#define THUMB_TOP  12          /* the picture: rows 12..21, up to 36 columns */
#define THUMB_ROWS 10
#define RUN_MAX    48

static void load_bank(FILE *f, int block, int n)
{
    static unsigned char run[RUN_MAX * 16];
    unsigned char e[17];
    int first = -1, len = 0;

    while (n-- > 0) {
        if (fread(e, 1, 17, f) != 17)
            break;
        if (len && (e[0] != first + len || len == RUN_MAX)) {
            vid_load_glyphs(block, (unsigned char)first, len, run);
            len = 0;
        }
        if (!len)
            first = e[0];
        memcpy(run + len * 16, e + 1, 16);
        len++;
    }
    if (len)
        vid_load_glyphs(block, (unsigned char)first, len, run);
}

/* the game's picture file, open and past its header, its glyphs loaded:
   its size in *cols, *rows; NULL when it has none (or the screen cannot
   show one) */
static FILE *thumb_open(const Game *g, int *cols, int *rows)
{
    char path[PATH_LEN + 24];
    FILE *f;
    unsigned char hdr[8];

    if (!vid_is_vga)
        return NULL;
    /* a picture made for this theme's colours first, then the plain one */
    sprintf(path, "%s%sTHUMBS\\%s\\%s.THM", home_dir,
            home_dir[strlen(home_dir) - 1] == '\\' ? "" : "\\", theme->name, g->dir);
    f = fopen(path, "rb");
    if (!f) {
        sprintf(path, "%s%sTHUMBS\\%s.THM", home_dir,
                home_dir[strlen(home_dir) - 1] == '\\' ? "" : "\\", g->dir);
        f = fopen(path, "rb");
    }
    if (!f) {                       /* one that came with the game, from the server */
        sprintf(path, "%s\\%s\\THUMBS\\%s.THM", gamedir, g->dir, theme->name);
        f = fopen(path, "rb");
    }
    if (!f)
        return NULL;
    if (fread(hdr, 1, 8, f) != 8 || memcmp(hdr, "W86T", 4) != 0 ||
        hdr[4] > PANE_W - 2 || hdr[5] > THUMB_ROWS) {
        fclose(f);
        return NULL;
    }
    *cols = hdr[4]; *rows = hdr[5];
    load_bank(f, 0, hdr[6]);
    load_bank(f, 1, hdr[7]);
    return f;
}

/* draw an opened picture, centred in the pane's picture rows, and close it */
static void thumb_draw(FILE *f, int cols, int rows)
{
    unsigned char cell[2];
    int x, y;
    int ox = PANE_X + 1 + (PANE_W - 2 - cols) / 2;
    int oy = THUMB_TOP + (THUMB_ROWS - rows) / 2;
    for (y = 0; y < rows; y++)
        for (x = 0; x < cols; x++) {
            if (fread(cell, 1, 2, f) != 2) { fclose(f); return; }
            scr_put(ox + x, oy + y, cell[0], cell[1]);
        }
    fclose(f);
}

/* ------------------------------------------------ the network view */

void ui_net_static(void)
{
    char t[24];

    scr_fill(0, 0, 80, 25, ' ', A(7, 0));
    draw_logo();
    draw_divider();
    draw_box(LIST_X, PANE_TOP, LIST_W, PANE_H, A(3, 0));
    sprintf(t, " NETWORK %d ", net_count);
    scr_puts(LIST_X + 2, PANE_TOP, t, A(11, 0));
    draw_box(PANE_X, PANE_TOP, PANE_W, PANE_H, A(3, 0));
    scr_puts(PANE_X + 2, PANE_TOP, " DOWNLOAD ", A(11, 0));
}

static void fmt_kb(char *dst, unsigned long kb)
{
    if (kb < 10000UL)
        sprintf(dst, "%luK", kb);
    else
        sprintf(dst, "%lu.%luM", kb / 1024, (kb % 1024) * 10 / 1024);
}

void ui_net_list(int sel, int top)
{
    int i;

    for (i = 0; i < LIST_ROWS; i++) {
        int gi = top + i;
        int y = 8 + i;
        scr_fill(LIST_X + 1, y, LIST_W - 2, 1, ' ', A(7, 0));
        if (gi >= net_count)
            continue;
        {
            const NetGame *g = net_get(gi);
            int is_sel = (gi == sel);
            unsigned char at = is_sel ? A(15, 3) : A(7, 0);
            unsigned char ad = is_sel ? A(0, 3) : A(8, 0);
            char nm[30], sz[12];

            if (is_sel) {
                scr_fill(LIST_X + 1, y, LIST_W - 2, 1, ' ', at);
                scr_put(LIST_X + 1, y, CH_ARROW, A(14, 3));
            }
            if (net_queued(g->dir))      /* waiting its turn to be fetched */
                scr_put(LIST_X + 2, y, 254, is_sel ? A(14, 3) : A(14, 0));
            strncpy(nm, g->title, 26);
            nm[26] = 0;
            scr_puts(LIST_X + 3, y, nm, at);
            fmt_kb(sz, net_size(g));
            scr_puts(LIST_X + LIST_W - 2 - strlen(sz), y, sz, ad);
        }
    }
    scr_put(LIST_X + LIST_W - 1, 8, top > 0 ? CH_UP : CH_V, A(11, 0));
    scr_put(LIST_X + LIST_W - 1, 8 + LIST_ROWS - 1,
            top + LIST_ROWS < net_count ? CH_DOWN : CH_V, A(11, 0));
    if (net_count == 0) {
        scr_puts(LIST_X + 4, 12, "NO GAME LIST YET", A(12, 0));
        scr_puts(LIST_X + 4, 14, "PRESS L TO FETCH IT FROM", A(7, 0));
        scr_puts(LIST_X + 4, 15, cfg_server[0] ? cfg_server : "(set server= in the INI)", A(11, 0));
    }
}

void ui_net_details(int sel)
{
    int y;
    char buf[48], sz[12];
    const NetGame *g;

    for (y = PANE_TOP + 1; y < PANE_TOP + PANE_H - 1; y++)
        scr_fill(PANE_X + 1, y, PANE_W - 2, 1, ' ', A(7, 0));
    if (net_count == 0) {
        scr_puts(PANE_X + 2, 9, "GAMES FROM YOUR COLLECTIONS,", A(8, 0));
        scr_puts(PANE_X + 2, 10, "SERVED BY WAVESERVE.PY.", A(8, 0));
        return;
    }
    g = net_get(sel);
    scr_puts(PANE_X + 2, 9, g->title, A(15, 0));
    scr_hline(PANE_X + 2, 10, strlen(g->title), CH_H, A(3, 0));
    if (g->year) sprintf(buf, "%u   ", g->year); else buf[0] = 0;
    strcat(buf, stricmp(g->src, "tdc") == 0 ? "TOTAL DOS COLLECTION" : "EXODOS");
    field(11, "FROM", buf, A(7, 0));
    sprintf(buf, "%s\\%s", gamedir, g->dir);
    if (strlen(buf) > PANE_W - 11) buf[PANE_W - 11] = 0;
    field(12, "TO", buf, A(7, 0));
    field(13, "EXEC", g->exe[0] ? g->exe : "(will be detected)", g->exe[0] ? A(7, 0) : A(8, 0));
    fmt_kb(sz, net_size(g));
    field(14, "SIZE", sz, A(7, 0));
    if (g->cd && g->netcd && net_cdmode)
        scr_puts(PANE_X + PANE_W - 2 - 10, 14, "\xAE NET CD \xAF", A(10, 0));
    else if (g->cd)
        scr_puts(PANE_X + PANE_W - 2 - 12, 14, "\xAE CD IMAGE \xAF", A(10, 0));
    else if (g->partial)
        scr_puts(PANE_X + PANE_W - 2 - 14, 14, "\xAE INCOMPLETE \xAF", A(12, 0));
    if (g->partial)
        scr_puts(PANE_X + 2, 19, "THE SERVER HAS ONLY PART OF IT.", A(8, 0));
    else if (g->cd && g->netcd && net_cdmode)
        scr_puts(PANE_X + 2, 19, "CD OVER NETWORK, C: ON DISK.", A(8, 0));
    else if (g->cd && g->netcd)
        scr_puts(PANE_X + 2, 19, "CD ON DISK, C: OVER NETWORK.", A(8, 0));
    else if (g->cd)
        scr_puts(PANE_X + 2, 19, "ITS CD IMAGE COMES ALONG.", A(8, 0));
    if (net_queued(g->dir))
        scr_puts(PANE_X + PANE_W - 2 - 10, 15, "\xAE QUEUED \xAF", A(14, 0));
    if (net_qcount) {
        char q[34];
        fmt_kb(sz, net_queue_kb());
        sprintf(q, "QUEUED: %d GAMES, %s. ENTER FETCHES", net_qcount, sz);
        scr_puts(PANE_X + 2, 20, q, A(14, 0));
        scr_puts(PANE_X + 2, 21, "THEM ALL; Q SHOWS THEM FIRST.", A(8, 0));
    } else if (g->netplay && wmode_network) {
        scr_puts(PANE_X + 2, 20, "ENTER PLAYS IT OFF THE SERVER,", A(8, 0));
        scr_puts(PANE_X + 2, 21, "I INSTALLS IT, SPACE QUEUES IT.", A(8, 0));
    } else if (g->netplay) {
        scr_puts(PANE_X + 2, 20, "ENTER INSTALLS IT, SPACE QUEUES IT,", A(8, 0));
        scr_puts(PANE_X + 2, 21, "P PLAYS IT OFF THE SERVER.", A(8, 0));
    } else {
        scr_puts(PANE_X + 2, 20, "ENTER INSTALLS IT INTO YOUR GAMES,", A(8, 0));
        scr_puts(PANE_X + 2, 21, "SPACE QUEUES IT FOR LATER.", A(8, 0));
    }
}

/* The menu: every key of the view, in two columns over the bottom of the
   screen, the chosen one on a bar; dim[i] greys an item that does nothing
   just now. The caller reads keys and turns a choice into that key. */
void ui_menu(const char *title, const MenuItem __far *items, int n, int sel, const unsigned char *dim)
{
    int i;
    char buf[8], key[6], label[32];

    scr_fill(0, MENU_Y, 80, 25 - MENU_Y, ' ', A(7, 0));
    draw_box(0, MENU_Y, 80, 25 - MENU_Y, A(11, 0));
    scr_puts(2, MENU_Y, title, A(15, 0));
    for (i = 0; i < n && i < 2 * MENU_ROWS; i++) {
        int x = 2 + (i / MENU_ROWS) * MENU_COLW, y = MENU_Y + 1 + i % MENU_ROWS;
        int is_sel = i == sel;
        /* the chosen one on the other shade, its key in gold on it */
        if (is_sel)
            scr_fill(x, y, MENU_COLW - 1, 1, ' ', A(15, 5));
        _fstrncpy(key, items[i].key, 5);
        key[5] = 0;
        _fstrncpy(label, items[i].label, 31);
        label[31] = 0;
        sprintf(buf, "%-5.5s", key);
        scr_puts(x, y, buf, dim[i] ? A(8, is_sel ? 5 : 0) : (is_sel ? A(14, 5) : A(0, 3)));
        scr_puts(x + 6, y, label, dim[i] ? A(8, is_sel ? 5 : 0) : (is_sel ? A(15, 5) : A(7, 0)));
    }
    {
        int x = 2, y = MENU_Y + MENU_ROWS + 2;
        keychip_at(&x, y, "ARROWS", "OPTIONS", 0);
        keychip_at(&x, y, "ENTER", "SELECT", 0);
        keychip_at(&x, y, "ESC", "RETURN", 0);
    }
}

/* A game's options, over the bottom of the screen like the menu: a few
   settings with their values, the chosen row on the other shade with
   arrows round its value. The caller turns keys into changes. */
void ui_options(const char *title, const char *const *labels, const char *const *values, int n, int sel)
{
    int i;
    scr_fill(0, MENU_Y, 80, 25 - MENU_Y, ' ', A(7, 0));
    draw_box(0, MENU_Y, 80, 25 - MENU_Y, A(11, 0));
    scr_puts(2, MENU_Y, title, A(15, 0));
    for (i = 0; i < n && i < MENU_ROWS; i++) {
        int y = MENU_Y + 1 + i, is_sel = i == sel;
        if (is_sel)
            scr_fill(2, y, 76, 1, ' ', A(15, 5));
        scr_puts(3, y, labels[i], is_sel ? A(14, 5) : A(3, 0));
        if (is_sel) {
            scr_put(24, y, 17, A(14, 5));                 /* the arrows: left, right */
            scr_puts(26, y, values[i], A(15, 5));
            scr_put(27 + strlen(values[i]), y, 16, A(14, 5));
        } else {
            scr_puts(26, y, values[i], A(7, 0));
        }
    }
    {
        int x = 2, y = MENU_Y + MENU_ROWS + 2;
        keychip_at(&x, y, "ARROWS", "CHOOSE", 0);
        keychip_at(&x, y, "ENTER", "SAVE", 0);
        keychip_at(&x, y, "ESC", "CANCEL", 0);
    }
}

/* the install queue in a box over the lists: what is in it, in the order
   it will be fetched, and its size */
#define QBOX_X 12
#define QBOX_W 67              /* over the whole details pane */

void ui_queue_box(int n, int sel, int top, const char __far *titles, const unsigned long __far *kbs)
{
    int shown = n < QBOX_ROWS ? n : QBOX_ROWS, i, h = shown + 4;   /* a blank line before the keys */
    char buf[QBOX_W], sz[12], title[QT_LEN];
    unsigned long total = 0;

    for (i = 0; i < n; i++)
        total += kbs[i];
    scr_fill(QBOX_X, QBOX_Y, QBOX_W, h, ' ', A(7, 0));
    draw_box(QBOX_X, QBOX_Y, QBOX_W, h, A(11, 0));
    fmt_kb(sz, total);
    sprintf(buf, " INSTALL QUEUE %c %d GAME%s, %s ", CH_DOT, n, n == 1 ? "" : "S", sz);
    scr_puts(QBOX_X + 2, QBOX_Y, buf, A(15, 0));
    for (i = 0; i < shown; i++) {
        int qi = top + i, y = QBOX_Y + 1 + i;
        int is_sel = qi == sel;
        unsigned char at = is_sel ? A(15, 5) : A(7, 0);     /* the other shade, like the menu */
        if (qi >= n) break;
        scr_fill(QBOX_X + 1, y, QBOX_W - 2, 1, ' ', at);
        if (is_sel)
            scr_put(QBOX_X + 2, y, CH_ARROW, A(14, 5));
        _fstrncpy(title, titles + qi * QROW, QT_LEN - 1);
        title[QT_LEN - 1] = 0;
        sprintf(buf, "%2d  %-32.32s", qi + 1, title);
        scr_puts(QBOX_X + 4, y, buf, at);
        fmt_kb(sz, kbs[qi]);
        scr_puts(QBOX_X + QBOX_W - 3 - strlen(sz), y, sz, is_sel ? A(14, 5) : A(8, 0));
    }
    scr_put(QBOX_X + QBOX_W - 1, QBOX_Y + 1, top > 0 ? CH_UP : CH_V, A(11, 0));
    scr_put(QBOX_X + QBOX_W - 1, QBOX_Y + shown, top + shown < n ? CH_DOWN : CH_V, A(11, 0));
    {
        int x = QBOX_X + 2, y = QBOX_Y + shown + 2;
        keychip_at(&x, y, "ENTER", "FETCH ALL", 0);
        keychip_at(&x, y, "DEL", "REMOVE ITEM", 0);
        keychip_at(&x, y, "ESC", "RETURN", 0);
    }
}

void ui_net_keybar(int cur)
{
    int x = 2;
    scr_fill(0, 23, 80, 1, ' ', A(7, 0));
    keychip(&x, "ENTER", wmode_network ? "PLAY" : "INSTALL", net_count == 0);
    keychip(&x, "SPACE", "QUEUE", net_count == 0);
    if (wmode_network)
        keychip(&x, "I", "INSTALL", net_count == 0);
    else
        keychip(&x, "P", "PLAY", net_count == 0 || !net_get(cur)->netplay);
    keychip(&x, "Q", "VIEW QUEUE", net_qcount == 0);
    keychip(&x, "/", "SEARCH", net_count == 0);
    keychip(&x, "M", "MENU", 0);        /* the rest is in there: C, I, L, U ... */
    keychip(&x, "ESC", "BACK", 0);
}
