"""Which part of Photo mode crashes a 64v64 match? Run by the harness as --then with big battles on.

    python -I photo_check.py <out-dir>

Waits for both teams to have 60+ live soldiers, then applies Photo mode's three parts one at a time, each through
the menu's own handler, and checks the game is still answering after each: free camera, HUD off (first use scans
memory), time scale 0 (first use scans memory). Writes <out-dir>/photo_check.json; the last step logged before the
game stops answering is the suspect.
"""
import json
import socket
import sys
import time

T0 = time.time()
LIMIT = 220


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
        except OSError as e:
            return 'down: %s' % e
        if not d:
            break
        out += d
        quiet = 0
    s.close()
    return out.decode('utf-8', 'replace').strip()


def main():
    out = sys.argv[1]
    if len(out) > 2 and out[0] == '/' and out[2] == '/':
        out = out[1].upper() + ':' + out[2:]
    log = []

    def note(step, reply):
        row = {'t': round(time.time() - T0, 1), 'clock': time.strftime('%H:%M:%S'), 'step': step, 'reply': reply[:300]}
        log.append(row)
        print(json.dumps(row), flush=True)
        with open(out + '/photo_check.json', 'w', encoding='utf-8') as f:
            json.dump(log, f, indent=1)                       # written after every step: the game may die mid-run

    def alive_for(seconds, step):
        """Ping every 2 s; False as soon as the game stops answering."""
        end = time.time() + seconds
        while time.time() < end:
            r = send('bots status')
            if r.startswith('down'):
                note(step + ': game stopped answering', r)
                return False
            time.sleep(2)
        note(step + ': still alive', send('bots status'))
        return True

    while time.time() - T0 < 60:                              # wait for the full battle
        st = send('bots status')
        note('battle', st)
        try:
            a = [int(x) for x in st.split('alive ')[1].split()[0].split('/')]
        except (IndexError, ValueError):
            a = [0, 0]
        if min(a) >= 60 or st.startswith('down'):
            break
        time.sleep(5)

    note('free camera on', send('ui do camera.freecam 1'))
    if not alive_for(30, 'free camera'):
        return
    note('hud off (first use scans memory)', send('ui do camera.hud 0'))
    if not alive_for(45, 'hud off'):
        return
    note('hud reply', send('ui do last camera.hud'))
    note('time scale 0 (first use scans memory)', send('ui do world.timescale 0'))
    if not alive_for(45, 'time scale 0'):
        return
    note('time scale reply', send('ui do last world.timescale'))
    note('restore', ' | '.join(send(c) for c in ('ui do world.timescale 1', 'ui do camera.hud 1', 'ui do camera.freecam 0')))
    note('done', 'all three parts survived')


if __name__ == '__main__':
    main()
