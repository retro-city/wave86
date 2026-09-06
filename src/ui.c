/*
 * ui.c - the synthwave text mode front end.
 * 80x25, direct video memory, CP437 box drawing, sunset gradients.
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <i86.h>
#include "wave86.h"

int ui_thumb(const Game *g);

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

static const char *logo[5] = {
    "#   #  ###  #   # #####         ###   ### ",
    "#   # #   # #   # #            #   # #    ",
    "# # # ##### #   # ####     #    ###  #### ",
    "# # # #   #  # #  #            #   # #   #",
    " # #  #   #   #   #####         ###   ### ",
};
static const unsigned char logo_clr[5] = { 14, 6, 12, 13, 5 };

static void draw_logo(void)
{
    int r, i;
    for (r = 0; r < 5; r++) {
        const char *s = logo[r];
        for (i = 0; s[i]; i++)
            if (s[i] == '#')
                scr_put(2 + i, 1 + r, CH_BLOCK, A(logo_clr[r], 0));
    }
    scr_puts(50, 1, "EPIC GAME LAUNCHER", A(11, 0));
    scr_puts(50, 2, cpu_desc[0] ? cpu_desc : "FOR 8086 AND UP", A(8, 0));
}

static void draw_divider(void)
{
    /* sunset gradient band, left to right */
    static const unsigned char band[6] = { 14, 6, 12, 13, 5, 1 };
    int x;
    for (x = 0; x < 80; x++)
        scr_put(x, 6, CH_HALF_HI, A(band[x * 6 / 80], 0));
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

static void keychip(int *x, const char *key, const char *label, int dim)
{
    scr_puts(*x, 23, key, dim ? A(8, 0) : A(0, 3));
    *x += strlen(key);
    scr_put(*x, 23, ' ', A(7, 0));
    *x += 1;
    scr_puts(*x, 23, label, dim ? A(8, 0) : A(7, 0));
    *x += strlen(label) + 2;
}

void ui_keybar(const Game *sel)
{
    int x = 2;
    char updown[4];

    scr_fill(0, 23, 80, 1, ' ', A(7, 0));
    updown[0] = CH_UP; updown[1] = CH_DOWN; updown[2] = 0;
    (void)updown;
    keychip(&x, "ENTER", "RUN", game_count == 0);
    keychip(&x, "S", "SETUP", !sel || !sel->setup[0]);
    keychip(&x, "F2", "NAME", game_count == 0);
    keychip(&x, "M", "MUSIC", !mus_present || !mus_ntracks);
    keychip(&x, "+-", "VOL", !mus_present || !mus_ntracks);
    keychip(&x, "<>", "TRACK", !mus_present || mus_ntracks < 2);
    keychip(&x, "R", "SCAN", 0);
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
    scr_puts(80 - 2 - 4, 24, "V" VERSION_STR, A(8, 0));
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
            unsigned char at = is_sel ? A(15, 5) : A(7, 0);
            unsigned char ad = is_sel ? A(11, 5) : A(8, 0);
            char nm[24];
            int dl;

            if (is_sel) {
                scr_fill(LIST_X + 1, y, LIST_W - 2, 1, ' ', at);
                scr_put(LIST_X + 1, y, CH_ARROW, A(14, 5));
            }
            strncpy(nm, g->name, 22);
            nm[22] = 0;
            scr_puts(LIST_X + 3, y, nm, at);
            dl = strlen(g->dir);
            scr_puts(LIST_X + LIST_W - 2 - dl, y, g->dir, ad);
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

void ui_details(int sel)
{
    int y;
    char buf[PATH_LEN + FN_LEN + 2];
    const Game *g;

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

    sprintf(buf, "%s\\%s", gamedir, g->dir);
    if (strlen(buf) > PANE_W - 11)
        buf[PANE_W - 11] = 0;
    field(11, "PATH", buf, A(7, 0));
    field(12, "EXEC", g->exe, A(7, 0));
    field(13, "SETUP", g->setup[0] ? g->setup : "none",
          g->setup[0] ? A(7, 0) : A(8, 0));
    if (g->args[0])
        field(15, "ARGS", g->args, A(7, 0));
    {
        const char *m = g->sound[0] ? g->sound : ini_global("sound");
        if (m && m[0]) {
            char up[16];
            int k;
            for (k = 0; m[k] && k < 15; k++)
                up[k] = (char)toupper((unsigned char)m[k]);
            up[k] = 0;
            field(14, "SOUND", up, A(7, 0));
        }
    }

    if (g->flags & GF_DOS4GW)
        scr_puts(PANE_X + 2, 18, "\xAE 386+ PROTECTED MODE \xAF", A(12, 0));

    if (!ui_thumb(g)) {
        scr_puts(PANE_X + 2, 20, "PRESS ENTER TO RUN THE GAME.", A(8, 0));
        scr_puts(PANE_X + 2, 21, "THE MENU RETURNS WHEN IT ENDS.", A(8, 0));
    }
}

/* F2: the name being typed, white on the selection colour, gold cursor */
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
 * The demoscene thumbnail: THUMBS\<DIR>.THM holds a 26x6 cell picture
 * whose cells are custom glyphs loaded into the VGA font RAM, two
 * colours per cell (see tools/makethumb.py). Text mode, no pixels.
 * Rows 16..21 of the details pane. Returns 1 when something was drawn.
 */
#define THUMB_X    (PANE_X + 2)
#define THUMB_Y    16
#define THUMB_COLS 26
#define THUMB_ROWS 6

int ui_thumb(const Game *g)
{
    char path[PATH_LEN + 24];
    FILE *f;
    unsigned char hdr[7];
    static unsigned char glyph[17];
    static unsigned char cell[2];
    int n, cols, rows, x, y;

    if (!vid_is_vga)
        return 0;
    sprintf(path, "%s%sTHUMBS\\%s.THM", home_dir,
            home_dir[strlen(home_dir) - 1] == '\\' ? "" : "\\", g->dir);
    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fread(hdr, 1, 7, f) != 7 || memcmp(hdr, "W86T", 4) != 0 ||
        hdr[4] > THUMB_COLS || hdr[5] > THUMB_ROWS) {
        fclose(f);
        return 0;
    }
    cols = hdr[4]; rows = hdr[5]; n = hdr[6];
    while (n-- > 0) {
        if (fread(glyph, 1, 17, f) != 17) break;
        vid_load_glyph(glyph[0], glyph + 1);
    }
    for (y = 0; y < rows; y++)
        for (x = 0; x < cols; x++) {
            if (fread(cell, 1, 2, f) != 2) { fclose(f); return 1; }
            scr_put(THUMB_X + x, THUMB_Y + y, cell[0], cell[1]);
        }
    fclose(f);
    return 1;
}
