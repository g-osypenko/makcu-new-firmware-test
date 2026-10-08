import csv, math

with open('telemetry_flight_log.csv', 'r', encoding='utf-8') as f:
    rows = list(csv.DictReader(f))

# Period is 2.0s = 1000 ticks.
# Let's fit circle (Xc, Yc, R) to each 1000-tick window:
# (x - Xc)^2 + (y - Yc)^2 = R^2
# Linear least squares: x^2 + y^2 = 2*x*Xc + 2*y*Yc + (R^2 - Xc^2 - Yc^2)
# A * [2Xc, 2Yc, C]^T = [x^2 + y^2]

def fit_circle(points):
    n = len(points)
    if n < 3:
        return 0, 0, 0
    sum_x = sum(p[0] for p in points)
    sum_y = sum(p[1] for p in points)
    sum_x2 = sum(p[0]**2 for p in points)
    sum_y2 = sum(p[1]**2 for p in points)
    sum_xy = sum(p[0]*p[1] for p in points)
    sum_x3 = sum(p[0]**3 for p in points)
    sum_y3 = sum(p[1]**3 for p in points)
    sum_xy2 = sum(p[0]*(p[1]**2) for p in points)
    sum_x2y = sum((p[0]**2)*p[1] for p in points)

    C = n * sum_x2 - sum_x**2
    D = n * sum_xy - sum_x * sum_y
    E = n * sum_x3 + n * sum_xy2 - (sum_x2 + sum_y2) * sum_x
    G = n * sum_y2 - sum_y**2
    H = n * sum_x2y + n * sum_y3 - (sum_x2 + sum_y2) * sum_y

    denom = 2 * (C * G - D**2)
    if abs(denom) < 1e-6:
        return 0, 0, 0
    a = (E * G - H * D) / denom
    b = (H * C - E * D) / denom
    r2 = (sum_x2 - 2*a*sum_x + n*a*a + sum_y2 - 2*b*sum_y + n*b*b) / n
    return a, b, math.sqrt(max(0, r2))

print("=== ФИТТИНГ ОКРУЖНОСТИ (Очистка от смещения руки) ===")
# Window of 1000 ticks (1 full period), stepped by 250 ticks (0.5s)
for start in range(0, len(rows) - 1000 + 1, 250):
    chunk = rows[start:start+1000]
    pts = [(int(r['cur_x']), int(r['cur_y'])) for r in chunk]
    xc, yc, radius = fit_circle(pts)
    t_start = float(chunk[0]['t_ms'])
    t_end = float(chunk[-1]['t_ms'])
    print(f"Период {t_start:5.0f}..{t_end:5.0f} мс (Такты {start:4d}..{start+1000:4d}): Центр=({xc:6.1f}, {yc:6.1f}) | РЕАЛЬНЫЙ РАДИУС R = {radius:5.1f} px")

