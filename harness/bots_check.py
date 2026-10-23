"""In-game check of the Bots tab / "bots" verbs (stage 2, Task 7). Run by the harness as --then while a level is up,
with settings.ini bigBattles=1 teamSize1=64 teamSize2=64 so gamelink fixed the cap at startup.

    python -I bots_check.py <out-dir>

Samples "bots status" (players and live soldiers per team, from the server's player list) every 15 s for 200 s,
grabs game-only frames with the Bots tab open, and writes <out-dir>/bots_check.json. Proof of 64v64 is the per-team
ALIVE count (spawned soldiers), not the player count.
"""
import json
import socket
import sys
import time

T0 = time.time()
LIMIT = 215


def send(cmd, timeout=15.0):
    try:
        s = socket.create_connection(('127.0.0.1', 31500), 5)
    except OSError as e:
        return 'down: %s' % e
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
    return out.decode('utf-8', 'replace').strip()


def parse(status):
    """bots on 64v64 players 64/63 alive 60/58 cap 130 ..."""
    w = status.split()
    try:
        p = [int(x) for x in w[w.index('players') + 1].split('/')]
        a = [int(x) for x in w[w.index('alive') + 1].split('/')]
        cap = int(w[w.index('cap') + 1])
        return p, a, cap
    except (ValueError, IndexError):
        return None, None, None


def main():
    out = sys.argv[1]
    if len(out) > 2 and out[0] == '/' and out[2] == '/':
        out = out[1].upper() + ':' + out[2:]
    win = out.replace('/', '\\')
    samples = []
    best_alive = [0, 0]
    shots = []
    send('ui open')
    send('ui tab bots')
    while time.time() - T0 < LIMIT - 20:
        st = send('bots status')
        p, a, cap = parse(st)
        row = {'t': round(time.time() - T0, 1), 'clock': time.strftime('%H:%M:%S'), 'status': st}
        if a:
            best_alive = [max(best_alive[0], a[0]), max(best_alive[1], a[1])]
        samples.append(row)
        print(json.dumps(row), flush=True)
        if len(samples) in (4, 9) or (a and min(a) >= 60 and len(shots) == 0):
            shots.append(send('grabframe %s\\bots_%02d.bmp' % (win, len(samples))))
        time.sleep(15)
    send('ui close')
    shots.append(send('grabframe %s\\bots_game.bmp' % win))
    ft = send('frametimes 10')
    last_p, last_a, cap = parse(samples[-1]['status']) if samples else (None, None, None)
    alive_ok = best_alive[0] >= 60 and best_alive[1] >= 60
    result = {
        'pass': bool(alive_ok and cap and cap >= 130 and last_p and min(last_p) >= 64 and 'refused 0' in samples[-1]['status']),
        'best_alive_per_team': best_alive,
        'last': samples[-1] if samples else None,
        'cap': cap,
        'frametimes': ft,
        'game_alive_at_end': send('ping').startswith('pong'),
        'shots': shots,
        'samples': samples,
        'seconds': round(time.time() - T0, 1),
    }
    with open(out + '/bots_check.json', 'w', encoding='utf-8') as f:
        json.dump(result, f, indent=1)
    print('PASS' if result['pass'] else 'FAIL', best_alive, cap, flush=True)


if __name__ == '__main__':
    main()
