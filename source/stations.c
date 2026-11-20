#include "stations.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u8 *db = NULL;
static int db_count = 0;
static u32 db_pool_size = 0;

#define DB_HDR 12
#define DB_REC 12

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static const u8 *rec_at(int i) { return db + DB_HDR + i * DB_REC; }
static const char *pool_str(u32 off)
{
    return (const char *)(db + DB_HDR + db_count * DB_REC + off);
}

static int try_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < DB_HDR) { fclose(f); return 0; }
    u8 *buf = (u8 *)malloc(size);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, size, f) != (size_t)size) { free(buf); fclose(f); return 0; }
    fclose(f);
    if (memcmp(buf, "STDB", 4) != 0) { free(buf); return 0; }
    int count = buf[6] | (buf[7] << 8);
    u32 pool = rd32(buf + 8);
    if (DB_HDR + (long)count * DB_REC + (long)pool != size) { free(buf); return 0; }
    db = buf;
    db_count = count;
    db_pool_size = pool;
    return 1;
}

int station_db_load(void)
{
    if (db) return db_count;
    if (try_load("sdmc:/ic-balance/stations.bin")) return db_count;
    if (try_load("romfs:/stations.bin")) return db_count;
    return -1;
}

// Exact (area,line,station) binary search over the sorted records.
static int lookup_exact(int area, int line, int sta,
                        const char **line_name, const char **sta_name)
{
    int lo = 0, hi = db_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const u8 *r = rec_at(mid);
        int ka = r[0], kl = r[1], ks = r[2];
        if (ka < area || (ka == area && kl < line) ||
            (ka == area && kl == line && ks < sta)) {
            lo = mid + 1;
        } else if (ka > area || (ka == area && kl > line) ||
                   (ka == area && kl == line && ks > sta)) {
            hi = mid - 1;
        } else {
            *line_name = pool_str(rd32(r + 4));
            *sta_name = pool_str(rd32(r + 8));
            return 1;
        }
    }
    return 0;
}

int station_resolve(int line, int sta, int region,
                    const char **line_name, const char **sta_name)
{
    if (!db) return 0;
    if (line == 0 && sta == 0) return 0; // empty field in the record
    // Area derivation (Cybernet standard): JR lines have line code < 0x80
    // and live in area 0; private-rail codes >= 0x80 are split by the
    // history block's region byte (0=Kanto area 1, 1=Kansai area 2, ...).
    int area;
    if (line < 0x80) area = 0;
    else if (region == 0) area = 1;
    else if (region == 1) area = 2;
    else if (region == 2) area = 3;
    else area = 1;
    if (lookup_exact(area, line, sta, line_name, sta_name)) return 1;
    // Fallback: same (line,station) under any other area (the DB's area
    // split is not perfectly aligned with every issuer's region byte).
    for (int i = 0; i < db_count; i++) {
        const u8 *r = rec_at(i);
        if (r[1] == line && r[2] == sta) {
            *line_name = pool_str(rd32(r + 4));
            *sta_name = pool_str(rd32(r + 8));
            return 1;
        }
    }
    return 0;
}
