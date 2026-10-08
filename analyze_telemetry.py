import csv

with open('telemetry_flight_log.csv', 'r', encoding='utf-8') as f:
    rows = list(csv.DictReader(f))

print("\n=== COMPARISON OF IDENTICAL COMMANDS: TICKS 0..1000 VS 1800..2800 ===")
sample_0 = [(r['tick'], r['dx_out'], r['dy_out'], r['scr_dx'], r['scr_dy']) 
            for r in rows[:1000] if int(r['dx_out']) == 3 and int(r['dy_out']) == 0]
sample_1 = [(r['tick'], r['dx_out'], r['dy_out'], r['scr_dx'], r['scr_dy']) 
            for r in rows[1800:2800] if int(r['dx_out']) == 3 and int(r['dy_out']) == 0]

print("Ticks 0..1000 for out=(3, 0):")
for s in sample_0[:10]:
    print(f"  tick={s[0]} out=({s[1]},{s[2]}) scr=({s[3]},{s[4]})")

print("Ticks 1800..2800 for out=(3, 0):")
for s in sample_1[:10]:
    print(f"  tick={s[0]} out=({s[1]},{s[2]}) scr=({s[3]},{s[4]})")

sample_neg_0 = [(r['tick'], r['dx_out'], r['dy_out'], r['scr_dx'], r['scr_dy']) 
                for r in rows[:1000] if int(r['dx_out']) == -3 and int(r['dy_out']) == 0]
sample_neg_1 = [(r['tick'], r['dx_out'], r['dy_out'], r['scr_dx'], r['scr_dy']) 
                for r in rows[1800:2800] if int(r['dx_out']) == -3 and int(r['dy_out']) == 0]

print("Ticks 0..1000 for out=(-3, 0):")
for s in sample_neg_0[:10]:
    print(f"  tick={s[0]} out=({s[1]},{s[2]}) scr=({s[3]},{s[4]})")

print("Ticks 1800..2800 for out=(-3, 0):")
for s in sample_neg_1[:10]:
    print(f"  tick={s[0]} out=({s[1]},{s[2]}) scr=({s[3]},{s[4]})")

