"""Talk to the injected FrostyGameLink on 127.0.0.1:31500 and aim its driven camera.

The link answers one command per connection and closes nothing, so a read loop that stops
after a quiet gap is the reliable framing -- the same rule viewer.ps1 uses.
"""
import os, socket, sys, time, math

def send(cmd, timeout=30.0):
    s = socket.create_connection(('127.0.0.1', 31500), 5)
    s.settimeout(0.4)
    s.sendall((cmd + '\n').encode('ascii'))
    out, deadline, quiet = b'', time.time() + timeout, 0
    while time.time() < deadline:
        try:
            d = s.recv(65536)
        except socket.timeout:
            quiet += 1
            if out and quiet >= 2:
                break
            continue
        if not d:
            break
        out += d
        quiet = 0
    s.close()
    return out.decode('ascii', 'replace').strip()

def camwhere():
    r = send('camwhere')
    n = [float(x) for x in __import__('re').findall(r'-?\d+\.\d+', r)]
    return (n[0:3], n[3:6]) if len(n) >= 6 else (None, None)

def aim(target, iters=4):
    """Turn the driven camera onto <target>. camlook is relative and its sign convention is
    not documented, so measure, step, and correct rather than trusting a formula."""
    for _ in range(iters):
        pos, fwd = camwhere()
        if pos is None:
            return 'no camera'
        d = [target[i] - pos[i] for i in range(3)]
        L = math.sqrt(sum(v * v for v in d)) or 1.0
        d = [v / L for v in d]
        want_yaw = math.degrees(math.atan2(d[0], d[2]))
        want_pitch = math.degrees(math.asin(max(-1.0, min(1.0, d[1]))))
        have_yaw = math.degrees(math.atan2(fwd[0], fwd[2]))
        have_pitch = math.degrees(math.asin(max(-1.0, min(1.0, fwd[1]))))
        dy = (want_yaw - have_yaw + 180) % 360 - 180
        dp = want_pitch - have_pitch
        if abs(dy) < 0.6 and abs(dp) < 0.6:
            break
        before = abs(dy) + abs(dp)
        send('camlook %.3f %.3f' % (dy, dp))
        pos, fwd = camwhere()
        hy = math.degrees(math.atan2(fwd[0], fwd[2]))
        hp = math.degrees(math.asin(max(-1.0, min(1.0, fwd[1]))))
        after = abs((want_yaw - hy + 180) % 360 - 180) + abs(want_pitch - hp)
        if after > before * 0.9:            # the step went the wrong way: flip and undo
            send('camlook %.3f %.3f' % (-2 * dy, -2 * dp))
    return send('camwhere')

HERE = os.path.dirname(os.path.abspath(__file__))
HASHES = [os.path.join(HERE, 'field_hashes.csv'),          # Frosty's names from the EBX dumps
          os.path.join(HERE, 'event_hashes_extra.csv')]    # harvest_*.py additions

def event_names():
    """hash -> name from the field-hash catalogue plus harvest_hashes.py's additions;
    the trace prints raw event ids."""
    names = {}
    for path in HASHES:
        try:
            for row in open(path, encoding='utf-8').read().splitlines()[1:]:
                parts = row.split(',')
                if len(parts) >= 3:
                    names.setdefault(parts[0][2:].lower().zfill(8), parts[2])
        except OSError:
            pass
    return names

def follow(filt='', interval=0.3, raw=False, feed='trace'):
    """Turn a feed on ('trace' = logic events, 'prop' = property values) and print it as it
    fires. The DLL already names what it knows; this fills in from the local tables too.
    Ctrl+C removes the hook. See TRACE.md."""
    import re
    names = {} if raw else event_names()
    tag = re.compile(r'\.([0-9a-f]{8})\b')
    print(send('%s on %s' % (feed, filt)))
    try:
        while True:
            reply = send('%s 2000' % feed, timeout=10)
            lines = reply.splitlines()
            for line in lines[1:]:
                print(tag.sub(lambda m: '.' + names.get(m.group(1), m.group(1)), line))
            if lines and lines[0].startswith('0 shown'):
                time.sleep(interval)
    except KeyboardInterrupt:
        print(send('%s off' % feed))

if __name__ == '__main__':
    if sys.argv[1] == 'follow':
        # gl.py follow [--prop] [--raw] [filter]   e.g.  gl.py follow --prop MosEspa,!Phantom
        rest = [a for a in sys.argv[2:] if a not in ('--raw', '--prop')]
        follow(' '.join(rest), raw='--raw' in sys.argv, feed='prop' if '--prop' in sys.argv else 'trace')
    elif sys.argv[1] == 'aim':
        print(aim([float(v) for v in sys.argv[2:5]]))
    else:
        print(send(' '.join(sys.argv[1:])))
