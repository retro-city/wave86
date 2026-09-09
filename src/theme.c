/*
 * theme.c - the looks. A theme is the logo (5 rows of '#'), its row
 * colours and shadow, the tagline, the divider band and the 16 DAC
 * colours. theme= in WAVE86.INI picks one; wave86 is the default.
 */
#include <string.h>
#include "wave86.h"

static const Theme themes[] = {
    { "wave86",
      { "#   #  ###  #   # #####         ###   ### ",
        "#   # #   # #   # #            #   # #    ",
        "# # # ##### #   # ####     #    ###  #### ",
        "# # # #   #  # #  #            #   # #   #",
        " # #  #   #   #   #####         ###   ### " },
      { 14, 6, 12, 13, 5 }, 0,
      "EPIC GAME LAUNCHER",
      { 14, 6, 12, 13, 5, 1 },
      { {  2,  0,  8 }, {  9,  4, 24 }, {  0, 34, 24 }, {  0, 46, 52 },
        { 52,  6, 22 }, { 44,  0, 42 }, { 58, 24,  2 }, { 36, 32, 46 },
        { 15,  9, 26 }, { 27, 24, 60 }, { 18, 60, 40 }, { 28, 63, 63 },
        { 63, 26, 34 }, { 63, 24, 56 }, { 63, 54, 16 }, { 62, 58, 63 } } },
    { "exodos",
      { "        D   D            CCCC    DDD    EEEE",
        " DDD     D D     DDD     C   C  D   D  E    ",
        "DDDDD     D     D   D    C   C  D   D   EEE ",
        "5        5 5    5   5    4   4  5   5      6",
        " 5555   5   5    555     4444    555   6666 " },
      { 13, 13, 13, 5, 5 }, 8,
      "THE EXODOS COLLECTION",
      { 13, 5, 12, 4, 6, 14 },
      /* the eXoDOS icon: violet and purple, a red D, an orange S */
      { {  3,  0,  6 }, { 16,  4, 30 }, {  0, 34, 18 }, { 34, 20, 50 },
        { 44,  4, 22 }, { 34,  8, 48 }, { 63, 34,  4 }, { 42, 38, 46 },
        { 18, 12, 26 }, { 34, 26, 63 }, { 24, 60, 30 }, { 52, 40, 63 },
        { 62, 14, 40 }, { 50, 28, 63 }, { 63, 50, 12 }, { 63, 62, 63 } } },
};

const Theme *theme = &themes[0];

void theme_select(const char *name)
{
    int i;
    theme = &themes[0];
    if (!name || !name[0])
        return;
    for (i = 0; i < (int)(sizeof(themes) / sizeof(themes[0])); i++)
        if (stricmp(themes[i].name, name) == 0)
            theme = &themes[i];
}
