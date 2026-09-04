/*
 * imf2wav - render a WAVE86 IMF file to a 16-bit mono WAV using AdPlug's
 * Nuked OPL emulator, so the soundtrack can be checked on the host.
 *
 *   c++ -O2 -std=c++17 -I/opt/homebrew/include -I/opt/homebrew/include/libbinio \
 *       -L/opt/homebrew/lib -ladplug -lbinio -o imf2wav imf2wav.cpp
 *
 *   imf2wav TRACK.IMF out.wav [rate_hz=560]
 */
#include <adplug/nemuopl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>

static const int SR = 44100;

static void wav_header(FILE *f, unsigned long nsamples)
{
    unsigned long datalen = nsamples * 2;
    unsigned char h[44];
    memcpy(h, "RIFF", 4);
    unsigned long riff = 36 + datalen;
    h[4] = riff; h[5] = riff >> 8; h[6] = riff >> 16; h[7] = riff >> 24;
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0;
    h[20] = 1; h[21] = 0;               /* PCM */
    h[22] = 1; h[23] = 0;               /* mono */
    h[24] = SR; h[25] = SR >> 8; h[26] = SR >> 16; h[27] = SR >> 24;
    unsigned long br = SR * 2;
    h[28] = br; h[29] = br >> 8; h[30] = br >> 16; h[31] = br >> 24;
    h[32] = 2; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    h[40] = datalen; h[41] = datalen >> 8; h[42] = datalen >> 16;
    h[43] = datalen >> 24;
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: imf2wav <in.imf> <out.wav> [rate]\n");
        return 2;
    }
    double rate = argc > 3 ? atof(argv[3]) : 560.0;

    FILE *in = fopen(argv[1], "rb");
    if (!in) { perror(argv[1]); return 1; }
    std::vector<unsigned char> d;
    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        d.insert(d.end(), buf, buf + n);
    fclose(in);

    /* same type-0 / type-1 detection as the DOS player */
    size_t pos = 0, end = d.size();
    if (d.size() >= 2) {
        unsigned w = d[0] | (d[1] << 8);
        if (w >= 4 && (w & 3) == 0 && w <= d.size() - 2) {
            pos = 2;
            end = 2 + w;
        }
    }

    CNemuopl opl(SR);
    opl.init();

    FILE *out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 1; }
    wav_header(out, 0);

    unsigned long total = 0;
    double carry = 0.0;
    std::vector<short> pcm;
    long peak = 0;
    double sumsq = 0.0;

    while (pos + 4 <= end) {
        unsigned char reg = d[pos], val = d[pos + 1];
        unsigned delay = d[pos + 2] | (d[pos + 3] << 8);
        pos += 4;
        opl.write(reg, val);
        double want = delay * (double)SR / rate + carry;
        long samples = (long)want;
        carry = want - samples;
        if (samples <= 0) continue;
        pcm.resize(samples);
        long done = 0;
        while (done < samples) {
            int chunk = (int)((samples - done) > 4096 ? 4096 : (samples - done));
            opl.update(pcm.data() + done, chunk);
            done += chunk;
        }
        for (long i = 0; i < samples; i++) {
            long v = pcm[i];
            if (v < 0) v = -v;
            if (v > peak) peak = v;
            sumsq += (double)pcm[i] * pcm[i];
        }
        fwrite(pcm.data(), 2, samples, out);
        total += samples;
    }
    wav_header(out, total);
    fclose(out);

    double rms = total ? sqrt(sumsq / total) : 0.0;
    printf("%s: %.1f s, peak %ld/32767, rms %.0f%s\n", argv[2],
           total / (double)SR, peak, rms,
           rms < 200 ? "  ** SUSPICIOUSLY QUIET **" : "");
    return 0;
}
