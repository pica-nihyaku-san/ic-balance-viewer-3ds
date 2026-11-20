// Station-name database for IC Balance Viewer.
// Loads stations.bin (SFCardFan-format data packed by db/convert.py):
//   header: 'STDB' u16 ver u16 count u32 poolSize
//   record x count (12B): u8 area,u8 line,u8 station,u8 pad, u32 lineNameOff, u32 stationNameOff
//   then the UTF-8 string pool.
// Lookup order: SD override (sdmc:/ic-balance/stations.bin) first so the
// database can be updated without reinstalling, then the bundled RomFS copy.
#pragma once
#include <3ds.h>

// Load the database. Returns the station count, or -1 when unavailable
// (the viewer then falls back to raw station codes).
int station_db_load(void);

// Resolve a station by line/station codes + the history entry's region byte.
// On success returns 1 and sets *line_name / *sta_name (owned by the DB).
int station_resolve(int line, int sta, int region,
                    const char **line_name, const char **sta_name);
