/* CLI entry point: argument parsing + usage - see README.md */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cli.h"

void usage(FILE *f)
{
    fprintf(f,
            "adofai-audio %s - lossless audio <-> .adofai \"audio-as-chart\" codec\n"
            "\n"
            "usage: adofai-audio <command> [options]\n"
            "\n"
            "commands:\n"
            "  encode INPUT [OUT]   audio -> .adofai (v2 change-event layout);\n"
            "                       OUT ending in .xz writes an xz-compressed chart\n"
            "      --artist NAME    override artist (default: ffprobe tags)\n"
            "      --title NAME     override song title\n"
            "      --out-dir DIR    directory for auto-named output\n"
            "      --xz-level L     xz preset 0-9 (optionally 9e); default 6\n"
            "  decode CHART OUT     .adofai or .xz -> mono s16 WAV\n"
            "      --gain F         output gain (1.0 = bit-exact)\n"
            "  verify CHART REF     bit-exact diff against reference audio\n"
            "  info CHART           print artist/song/author + audio info\n"
            "  self-test            roundtrip a synthetic tone\n"
            "\n"
            "options:\n"
            "  -h, --help           show this help\n"
            "      --version        print version\n"
            "\n"
            "format: bpm = sampleRate*60; hitsoundVolume = int16/655.36;\n"
            "events only at sample changes; angleData is one dense zero line.\n"
            "decode/verify/info detect .xz by content and decompress on the fly.\n"
            "ffmpeg/ffprobe are needed only for non-WAV input or tags.\n",
            VERSION);
}

int main(int argc, char **argv)
{
    const char *cmd = NULL;
    const char *pos[4] = {0};
    int npos = 0;
    const char *artist = NULL, *title = NULL, *outdir = NULL, *xzopt = NULL;
    double gain = 1.0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "--version") == 0) {
            printf("adofai-audio %s\n", VERSION);
            return 0;
        } else if (strcmp(a, "--artist") == 0 && i + 1 < argc) {
            artist = argv[++i];
        } else if (strcmp(a, "--title") == 0 && i + 1 < argc) {
            title = argv[++i];
        } else if (strcmp(a, "--out-dir") == 0 && i + 1 < argc) {
            outdir = argv[++i];
        } else if (strcmp(a, "--xz-level") == 0 && i + 1 < argc) {
            xzopt = argv[++i];
        } else if (strcmp(a, "--gain") == 0 && i + 1 < argc) {
            gain = atof(argv[++i]);
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "unknown option: %s\n", a);
            usage(stderr);
            return 2;
        } else if (!cmd) {
            cmd = a;
        } else if (npos < 4) {
            pos[npos++] = a;
        } else {
            fprintf(stderr, "too many arguments\n");
            return 2;
        }
    }
    if (!cmd) {
        usage(stderr);
        return 2;
    }
    if (strcmp(cmd, "encode") == 0) {
        if (npos < 1 || npos > 2) { usage(stderr); return 2; }
        return encode_cmd(pos[0], npos == 2 ? pos[1] : NULL, outdir, title, artist, xzopt);
    }
    if (strcmp(cmd, "decode") == 0) {
        if (npos != 2) { usage(stderr); return 2; }
        return decode_cmd(pos[0], pos[1], gain);
    }
    if (strcmp(cmd, "verify") == 0) {
        if (npos != 2) { usage(stderr); return 2; }
        return verify_cmd(pos[0], pos[1]);
    }
    if (strcmp(cmd, "info") == 0) {
        if (npos != 1) { usage(stderr); return 2; }
        return info_cmd(pos[0]);
    }
    if (strcmp(cmd, "self-test") == 0) {
        return selftest_cmd();
    }
    fprintf(stderr, "unknown command: %s\n", cmd);
    usage(stderr);
    return 2;
}

