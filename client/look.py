"""Put the driven camera at <cam> looking at <tgt>, then leave it there for a screenshot.

  python look.py camx camy camz tgtx tgty tgtz

watchdrive owns the position (camto), camlock owns the orientation (camlook, which is relative and
whose sign convention is not documented -- so measure, step, and undo an overshoot).
"""
import re, math, sys
from gl import send

def fwd():
    n = [float(x) for x in re.findall(r'-?\d+\.\d+', send('camlook 0 0'))]
    return n[:3] if len(n) >= 3 else None

def go(cam, tgt):
    send('watchdrive 1'); send('camlock 1')
    send('camto %f %f %f' % cam)
    d = [tgt[i] - cam[i] for i in range(3)]
    L = math.sqrt(sum(v * v for v in d)) or 1.0
    d = [v / L for v in d]
    # camlook reports the NEGATIVE of the view direction: a reported forward of
    # (0, -0.98, 0) renders as looking straight up. Aim at -d, not d.
    wy = math.degrees(math.atan2(-d[0], -d[2]))
    # camlook reports a forward whose Y is negated relative to what the view actually shows:
    # a reported -0.98 renders as looking straight UP. Aim in reported space, not view space.
    wp = -math.degrees(math.asin(max(-1.0, min(1.0, d[1]))))
    for _ in range(6):
        f = fwd()
        if not f:
            return 'no camera'
        hy = math.degrees(math.atan2(f[0], f[2]))
        hp = math.degrees(math.asin(max(-1.0, min(1.0, f[1]))))
        dy = (wy - hy + 180) % 360 - 180
        dp = wp - hp
        if abs(dy) < 1 and abs(dp) < 1:
            break
        send('camlook %.3f %.3f' % (dy, dp))
        f2 = fwd()
        hy2 = math.degrees(math.atan2(f2[0], f2[2]))
        hp2 = math.degrees(math.asin(max(-1.0, min(1.0, f2[1]))))
        if abs((wy - hy2 + 180) % 360 - 180) + abs(wp - hp2) > abs(dy) + abs(dp):
            send('camlook %.3f %.3f' % (-2 * dy, -2 * dp))
    send('camto %f %f %f' % cam)
    return send('camlook 0 0')

if __name__ == '__main__':
    v = [float(x) for x in sys.argv[1:7]]
    print(go(tuple(v[:3]), tuple(v[3:])))
