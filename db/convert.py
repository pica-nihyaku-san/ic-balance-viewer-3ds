# -*- coding: utf-8 -*-
"""StationCode.csv (SFCardFan format, Shift_JIS) -> stations.bin for the 3DS app.
Format (LE): header 'STDB' u16 ver=1 u16 count u32 poolSize
record x count (8B): u8 area,u8 line,u8 station,u8 0, u32 off -> pool: lineName\0stationName\0
records sorted by (area,line,station)."""
import csv, io, struct, sys

raw = open('StationCode.csv','rb').read().decode('shift_jis', errors='replace')
rows = [r for r in csv.reader(io.StringIO(raw)) if len(r)>=6 and r[0].strip().isdigit()]

def fw_score(s):  # fewer full-width latin chars = better row
    return sum(1 for c in s if 'Ａ' <= c <= 'ｚ')

best = {}
for r in rows:
    area, line, sta = int(r[0]), int(r[1]), int(r[2])
    company, linename, staname = r[3].strip(), r[4].strip(), r[5].strip()
    if company == '試験' or not staname: continue
    key = (area, line, sta)
    cur = best.get(key)
    if cur is None or fw_score(company) < fw_score(cur[0]):
        best[key] = (company, linename, staname)

def fix_line(name):
    if not name: return name
    if name.endswith(('線','新幹線','鉄道','ライン','モノレール','ケーブル')): return name
    return name + '線'

pool = bytearray(b'\x00')
offmap = {}
def intern(s):
    if s in offmap: return offmap[s]
    off = len(pool); pool.extend(s.encode('utf-8')); pool.append(0)
    offmap[s] = off; return off

recs = []
for key in sorted(best):
    company, linename, staname = best[key]
    lo = intern(fix_line(linename)); so = intern(staname)
    recs.append((key, lo, so))

out = bytearray()
out += b'STDB' + struct.pack('<HHI', 1, len(recs), len(pool))
for (a,l,s), lo, so in recs:
    out += struct.pack('<BBBBII', a, l, s, 0, lo, so)
out += pool
open('stations.bin','wb').write(out)
print(f"records={len(recs)} pool={len(pool)} total={len(out)} bytes")
