/* CLI entry point: ffmpeg-style argument parsing + usage - see README.md */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "cli.h"

void usage(FILE *f)
{
    fprintf(f,
            "adofai-music %s - lossless audio <-> .adofai \"audio-as-chart\" codec\n"
            "\n"
            "usage: adofai-music [options] -i INPUT [OUTPUT]\n"
            "\n"
            "operation (inferred from OUTPUT, or pick one flag):\n"
            "  INPUT audio  OUTPUT .adofai/.xz/.zst   audio -> chart (encode)\n"
            "  INPUT chart  OUTPUT .wav               chart -> WAV  (decode)\n"
            "  -show          print chart metadata + audio info\n"
            "  -play          play chart on the default audio device\n"
            "  -verify REF    bit-exact compare chart samples against audio REF\n"
            "  -bench         per-preset compression size/speed (in memory)\n"
            "  -self-test     in-memory roundtrip of a synthetic tone\n"
            "\n"
            "options:\n"
            "  -i FILE        input (audio file, or .adofai/.xz/.zst chart)\n"
            "  -metadata k=v  encode metadata: song= / artist= (default: ffprobe)\n"
            "  -xz-level L    xz preset 0-9 (optionally 9e); default 6 (9e = smallest)\n"
            "  -zstd-level L  zstd level 1-22; default 19 (3 = fastest)\n"
            "  -gain F        decode/play gain (1.0 = bit-exact)\n"
            "  -slice MB      bench chart text slice; default 256\n"
            "  -full          bench the whole chart text\n"
            "  -y / -n        overwrite output / never overwrite (default: -n)\n"
            "  -loglevel L    quiet | info (default) | verbose\n"
            "  -hide_banner   suppress the version banner\n"
            "  -h, -help      show this help\n"
            "  -version       print version\n"
            "\n"
            "format: bpm = sampleRate*60; hitsoundVolume = int16/655.36;\n"
            "events only at sample changes; angleData is one dense zero line.\n"
            "show/play/verify/decode detect .xz/.zst by content.\n"
            "ffmpeg/ffprobe are needed only for non-WAV input or tags.\n",
            VERSION);
}

static int ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcasecmp(s + ls - lf, suffix) == 0;
}

/* skip leading '-' so -gain and --gain are both accepted */
static const char *optname(const char *a) { return a + strspn(a, "-"); }

static void set_metadata(const char *kv, char **song, char **artist)
{
    const char *eq = strchr(kv, '=');
    if (!eq) die("bad -metadata '%s' (use key=value)", kv);
    size_t klen = (size_t)(eq - kv);
    const char *val = eq + 1;
    if ((klen == 4 && strncasecmp(kv, "song", 4) == 0) ||
        (klen == 5 && strncasecmp(kv, "title", 5) == 0)) { *song = (char *)val; return; }
    if (klen == 6 && strncasecmp(kv, "artist", 6) == 0) { *artist = (char *)val; return; }
    die("unsupported -metadata key '%.*s' (use song= / artist=)", (int)klen, kv);
}

static int parse_loglevel(const char *v)
{
    if (strcasecmp(v, "quiet") == 0 || strcmp(v, "0") == 0) return 0;
    if (strcasecmp(v, "info") == 0 || strcmp(v, "1") == 0) return 1;
    if (strcasecmp(v, "verbose") == 0 || strcmp(v, "2") == 0) return 2;
    die("bad -loglevel '%s' (quiet | info | verbose)", v);
    return 1;
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL;
    char *song = NULL, *artist = NULL;
    const char *xzopt = NULL, *zstdopt = NULL, *verify_ref = NULL;
    double gain = 1.0;
    long slice_mb = 256;
    int full = 0, force = 0, banner = 1;
    int ops = 0, nplay = 0, nverify = 0, nbench = 0, nselftest = 0, nshow = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *o = optname(a);
        if (*a != '-') {
            if (out) die("only one output is supported (got '%s' and '%s')", out, a);
            out = a;
        } else if (!strcmp(o, "i") && i + 1 < argc) {
            if (in) die("only one -i input is supported");
            in = argv[++i];
        } else if (!strcmp(o, "metadata") && i + 1 < argc) {
            set_metadata(argv[++i], &song, &artist);
        } else if (!strcmp(o, "verify") && i + 1 < argc) {
            verify_ref = argv[++i]; nverify++;
        } else if (!strcmp(o, "xz-level") && i + 1 < argc) {
            xzopt = argv[++i];
        } else if (!strcmp(o, "zstd-level") && i + 1 < argc) {
            zstdopt = argv[++i];
        } else if (!strcmp(o, "gain") && i + 1 < argc) {
            gain = atof(argv[++i]);
        } else if (!strcmp(o, "slice") && i + 1 < argc) {
            slice_mb = atol(argv[++i]);
        } else if (!strcmp(o, "full")) {
            full = 1;
        } else if (!strcmp(o, "show")) {
            nshow++;
        } else if (!strcmp(o, "play")) {
            nplay++;
        } else if (!strcmp(o, "bench")) {
            nbench++;
        } else if (!strcmp(o, "self-test")) {
            nselftest++;
        } else if (!strcmp(o, "y")) {
            force = 1;
        } else if (!strcmp(o, "n")) {
            force = 0;
        } else if (!strcmp(o, "loglevel") && i + 1 < argc) {
            log_level = parse_loglevel(argv[++i]);
        } else if (!strcmp(o, "hide_banner")) {
            banner = 0;
        } else if (!strcmp(o, "h") || !strcmp(o, "help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(o, "version")) {
            printf("adofai-music %s\n", VERSION);
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", a);
            usage(stderr);
            return 2;
        }
    }

    ops = nshow + nplay + nverify + nbench + nselftest;
    if (ops > 1) die("pick only one of -show/-play/-verify/-bench/-self-test");

    if (banner) log_msg(1, "adofai-music %s\n", VERSION);

    if (nselftest) {
        log_verbose("operation: self-test\n");
        return selftest_cmd();
    }
    if (!in) die("no input: use -i INPUT (see -help)");
    if (nshow) {
        if (out) die("-show does not take an output");
        log_verbose("operation: show\n");
        return info_cmd(in);
    }
    if (nplay) {
        if (out) die("-play does not take an output");
        log_verbose("operation: play, gain %.2f\n", gain);
        return play_cmd(in, gain);
    }
    if (nverify) {
        if (out) die("-verify does not take an output");
        log_verbose("operation: verify against %s\n", verify_ref);
        return verify_cmd(in, verify_ref);
    }
    if (nbench) {
        if (out) die("-bench does not take an output");
        log_verbose("operation: bench, slice %ld MiB%s\n", slice_mb, full ? " (full)" : "");
        return bench_cmd(in, slice_mb, full);
    }

    if (!out) die("no output: add OUTPUT, or use -show/-play/-verify/-bench");

    int decode = ends_with(out, ".wav");
    int encode = !decode && (ends_with(out, ".adofai") || ends_with(out, ".xz") ||
                             ends_with(out, ".zst") || ends_with(out, ".zstd"));
    if (!decode && !encode)
        die("unknown output format: %s (use .adofai/.xz/.zst, or .wav)", out);
    if (!force && access(out, F_OK) == 0)
        die("output %s exists; add -y to overwrite", out);

    if (decode) {
        log_verbose("operation: decode (chart -> wav), gain %.2f\n", gain);
        return decode_cmd(in, out, gain);
    }
    log_verbose("operation: encode (audio -> chart)\n");
    return encode_cmd(in, out, song, artist, xzopt, zstdopt);
}
