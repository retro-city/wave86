/*
 * scene2imf - convert demoscene AdLib music to WAVE86 IMF (140 Hz).
 *
 * Uses libAdPlug to play RAD, D00, HSC, A2M, S3M(AdLib), SA2, CFF, ...
 * and captures the OPL register writes with timestamps.
 *
 *   c++ -O2 -std=c++17 -I/opt/homebrew/include -L/opt/homebrew/lib \
 *       -ladplug -o scene2imf scene2imf.cpp
 *
 *   scene2imf TUNE.RAD ../music/TUNE.IMF [max_seconds]
 */
#include <adplug/adplug.h>
#include <adplug/opl.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

static const double TICK_HZ = 560.0;     /* id IMF standard rate */
static const size_t MAX_BYTES = 240000;  /* WAVE86 player buffer cap */

struct Ev { double t; unsigned char reg, val; };

class LogOpl : public Copl {
public:
    std::vector<Ev> evs;
    double now = 0.0;

    LogOpl() { currType = TYPE_OPL2; }

    void write(int reg, int val) override {
        if (currChip != 0)
            return;                      /* OPL3 second bank: drop */
        if (reg < 0x01 || reg > 0xF5 ||
            reg == 0x02 || reg == 0x03 || reg == 0x04)
            return;                      /* skip timer registers */
        evs.push_back({now, (unsigned char)reg, (unsigned char)val});
    }
    void init() override {}
};

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: scene2imf <tune> <out.imf> [max_sec]\n");
        return 2;
    }
    double max_sec = argc > 3 ? atof(argv[3]) : 180.0;

    LogOpl opl;
    CPlayer *p = CAdPlug::factory(argv[1], &opl);
    if (!p) {
        fprintf(stderr, "%s: format not recognized by AdPlug\n", argv[1]);
        return 1;
    }
    printf("%s: %s", argv[1], p->gettype().c_str());
    if (!p->gettitle().empty())
        printf(" - \"%s\"", p->gettitle().c_str());
    printf("\n");

    bool more = true;
    while (more && opl.now < max_sec) {
        more = p->update();
        float r = p->getrefresh();
        opl.now += 1.0 / (r > 1.0f ? r : 18.2f);
    }

    /* quantize to 140 Hz and emit reg,val,delay16 records */
    std::vector<unsigned char> out;
    size_t n = opl.evs.size(), i = 0;
    bool truncated = false;
    for (i = 0; i < n; i++) {
        long tick = (long)(opl.evs[i].t * TICK_HZ + 0.5);
        long next = (i + 1 < n) ? (long)(opl.evs[i + 1].t * TICK_HZ + 0.5)
                                : tick + (long)TICK_HZ;
        long delay = next - tick;
        if (delay < 0) delay = 0;
        if (delay > 0xFFFF) delay = 0xFFFF;
        if (out.size() + 4 * 12 > MAX_BYTES) {  /* room for the tail */
            truncated = true;
            break;
        }
        out.push_back(opl.evs[i].reg);
        out.push_back(opl.evs[i].val);
        out.push_back((unsigned char)(delay & 0xFF));
        out.push_back((unsigned char)(delay >> 8));
    }
    /* tail: all notes off + a one second gap */
    for (int ch = 0; ch < 9; ch++) {
        out.push_back((unsigned char)(0xB0 + ch));
        out.push_back(0);
        out.push_back(0); out.push_back(0);
    }
    out.push_back(0xBD); out.push_back(0);
    out.push_back((unsigned char)((long)TICK_HZ & 0xFF));
    out.push_back(0);

    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    if (out.size() <= 0xFFFE) {
        /* small enough for a standard type-1 IMF (16-bit length) */
        unsigned char hdr[2] = { (unsigned char)(out.size() & 0xFF),
                                 (unsigned char)(out.size() >> 8) };
        fwrite(hdr, 1, 2, f);
    } else {
        /* too big for type-1: type-0, opened with a dummy record so
           the first word reads 0 */
        unsigned char dummy[4] = { 0, 0, 0, 0 };
        fwrite(dummy, 1, 4, f);
    }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);

    printf("%s: %zu bytes, %.0fs, %zu/%zu events%s\n", argv[2],
           out.size(), i ? opl.evs[i - 1].t : 0.0, i, n,
           truncated ? " (TRUNCATED at 240K player cap)" : "");
    return 0;
}
