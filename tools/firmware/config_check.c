/* SPDX-License-Identifier: MIT
 * Validate configuration tar.gz before BusyBox extracts it. No links, devices,
 * executable restore hooks, vendor assets or paths outside persistent settings.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#define LIMIT (3 * 1024 * 1024)
static void fail(const char *s) { fprintf(stderr, "Invalid configuration backup: %s\n", s); exit(1); }
static unsigned long octal(const unsigned char *p, size_t n) {
    unsigned long v = 0;
    size_t i = 0;
    while (i < n && p[i] == ' ') i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
        if (v > LIMIT) fail("numeric field too large");
        v = v * 8 + p[i] - '0';
    }
    for (; i < n; i++) if (p[i] && p[i] != ' ') fail("invalid octal field");
    return v;
}
static int allowed(const char *name, int dir) {
    const char *leaf;
    const char *dirs[] = { "etc/config", "etc/dropbear", "etc/crontabs", "etc/rax-services" };
    const char *files[] = { "etc/passwd", "etc/group", "etc/shadow", "etc/rc.local", "etc/sysupgrade.conf" };
    if (dir && !strcmp(name, "etc")) return 1;
    for (size_t i = 0; i < sizeof(dirs)/sizeof(*dirs); i++) {
        size_t n = strlen(dirs[i]);
        if (dir && !strcmp(name, dirs[i])) return 1;
        if (!strncmp(name, dirs[i], n) && name[n] == '/') {
            leaf = name + n + 1;
            if (!*leaf) return 0;
            while (*leaf) {
                size_t len = strcspn(leaf, "/");
                if (!len || (len == 1 && *leaf == '.') || (len == 2 && !strncmp(leaf, "..", 2))) return 0;
                for (size_t j = 0; j < len; j++)
                    if ((unsigned char)leaf[j] < 32 || leaf[j] == '\\') return 0;
                leaf += len;
                if (*leaf) { leaf++; if (!*leaf) return 0; }
            }
            return 1;
        }
    }
    if (!dir) for (size_t i = 0; i < sizeof(files)/sizeof(*files); i++)
        if (!strcmp(name, files[i])) return 1;
    return 0;
}
int main(int argc, char **argv) {
    unsigned char h[512], block[512];
    unsigned long total = 0, data = 0;
    int format = 0, zeros = 0, files = 0;
    struct stat st;
    if (argc != 2) fail("expected archive path");
    if (lstat(argv[1], &st) || !S_ISREG(st.st_mode) || st.st_size > 2*1024*1024) fail("archive size/type");
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(h, 1, 2, f) != 2 || h[0] != 0x1f || h[1] != 0x8b) fail("expected gzip");
    fclose(f);
    gzFile g = gzopen(argv[1], "rb");
    if (!g) fail("cannot open gzip");
    for (;;) {
        int got = gzread(g, h, sizeof(h));
        if (got == 0) break;
        if (got != 512 || (total += 512) > LIMIT) fail("truncated or oversized tar");
        unsigned sum = 0;
        for (int i = 0; i < 512; i++) sum += h[i];
        if (!sum) { zeros++; continue; }
        if (zeros) fail("data after tar terminator");
        unsigned check = 0;
        for (int i = 0; i < 512; i++) check += (i >= 148 && i < 156) ? ' ' : h[i];
        if (check != octal(h + 148, 8)) fail("tar header checksum");
        if (memcmp(h + 257, "ustar", 5)) fail("expected ustar format");
        if (!memchr(h, 0, 100) || !memchr(h + 345, 0, 155)) fail("long path");
        char name[256];
        snprintf(name, sizeof(name), "%.154s%s%.99s", h + 345, h[345] ? "/" : "", h);
        char *p = name;
        if (!strncmp(p, "./", 2)) p += 2;
        int dir = h[156] == '5';
        size_t n = strlen(p);
        if (dir && n && p[n-1] == '/') p[n-1] = 0;
        if (h[156] && h[156] != '0' && !dir) fail("only regular files and directories are supported");
        int marker = !strcmp(p, "etc/rax-backup-format") && !dir;
        if (!marker && !allowed(p, dir)) { fprintf(stderr, "Unsupported backup path: %s\n", p); return 1; }
        unsigned long size = octal(h + 124, 12);
        if ((dir && size) || (data += size) > 2*1024*1024) fail("configuration too large");
        for (unsigned long left = (size + 511) / 512; left; left--) {
            if (gzread(g, block, 512) != 512 || (total += 512) > LIMIT) fail("truncated file");
            if (marker) {
                const char magic[] = "rax3000h-config-v1\n";
                if (size != sizeof(magic)-1 || memcmp(block, magic, sizeof(magic)-1)) fail("unknown backup format");
                format = 1;
            }
        }
        if (!dir) {
            files++;
            puts(p);
        }
    }
    int err;
    gzerror(g, &err);
    if (err != Z_OK && err != Z_STREAM_END) fail("gzip integrity check");
    if (gzclose(g) != Z_OK || zeros < 2 || !files || !format) fail("missing RAX3000H backup format identifier");
    return 0;
}
