"""In-game check of the F1 menu plumbing (stage 1, Task 4). Run by the harness as --then while a level is up.

    python -I ui_check.py <out-dir>

Drives the menu through the `ui` verb (real keyboard/mouse blocking is checked by a person: synthetic input passes
the raw-input hooks), grabs a game-only frame with the menu open, and writes <out-dir>/ui_check.json with every
reply and a pass/fail per step. Hard limit 200 s.
"""
import json
import socket
import sys
import time

T0 = time.time()
LIMIT = 200
LOG = 'C:/ProgramData/FrostyGameLink/gamelink.log'


def send(cmd, timeout=15.0):
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
    return out.decode('utf-8', 'replace').strip()


def log_lines(needle):
    try:
        found = []
        with open(LOG, encoding='utf-8', errors='replace') as f:       # streamed: the log spans every run
            for line in f:
                if 'FrostyGameLink attached' in line:
                    found = []                                          # this run only
                elif needle in line:
                    found.append(line.rstrip())
        return found
    except OSError:
        return []


def FrameStatsFps(reply):
    """The fps figure from a 'frametimes' reply (0 when there is none)."""
    try:
        return float(reply.split(' fps')[0].split(', ')[-1])
    except (IndexError, ValueError):
        return 0.0


def main():
    out = sys.argv[1]
    if len(out) > 2 and out[0] == '/' and out[2] == '/':        # an MSYS path (/d/...) from bash
        out = out[1].upper() + ':' + out[2:]
    steps = []

    def step(name, cmd, ok):
        reply = send(cmd)
        steps.append({'step': name, 'cmd': cmd, 'reply': reply, 'pass': bool(ok(reply))})
        print(json.dumps(steps[-1]), flush=True)
        return reply

    # The boot thread installs the hooks a few seconds after the window appears; wait for both.
    while time.time() - T0 < 90:
        if log_lines('menu: present hook') and log_lines('inputhook:'):
            break
        time.sleep(2)
    first = send('ui status')
    steps.append({'step': 'state at start (a person may have pressed F1)', 'reply': first, 'pass': True})
    step('closes', 'ui close', lambda r: r == 'ui closed')
    step('opens', 'ui open', lambda r: r.startswith('ui open tab ') and 'input blocked' in r)
    step('tab by name', 'ui tab bots', lambda r: r.startswith('ui open tab Bots'))
    time.sleep(1.5)                               # let a few frames draw the menu
    step('frame with menu', 'grabframe %s\\menu_open.bmp' % out.replace('/', '\\'), lambda r: r.startswith('captured'))
    step('focus loss closes', 'ui test-focuslost', lambda r: r == 'ui closed')
    step('toggle reopens on the same tab', 'ui toggle', lambda r: r.startswith('ui open tab Bots'))
    step('close', 'ui close', lambda r: r == 'ui closed')
    step('bad tab rejected', 'ui tab nope', lambda r: r.startswith('ui: unknown tab'))
    time.sleep(1.0)
    step('frame closed (badge)', 'grabframe %s\\menu_closed.bmp' % out.replace('/', '\\'), lambda r: r.startswith('captured'))

    # ---- Task 5: the tabs' handlers, through the same code the widgets run ----
    def settle(control, limit=45.0):
        """The control's newest reply once the menu's command thread has run it (or the last answer at the limit)."""
        t = time.time()
        r = send('ui do last ' + control)
        while 'still working' in r and time.time() - t < limit:
            time.sleep(0.25)
            r = send('ui do last ' + control)
        return r

    step('open on World', 'ui open', lambda r: r.startswith('ui open'))
    send('ui tab world')
    time.sleep(3.0)                               # level name refresh is every 2 s
    step('frame World tab', 'grabframe %s\\menu_world.bmp' % out.replace('/', '\\'), lambda r: r.startswith('captured'))
    steps.append({'step': 'queue after World tab', 'reply': send('ui do queue'), 'pass': True})
    step('time scale', 'ui do world.timescale 0.25', lambda r: r.endswith('queued'))
    r = settle('world.timescale')
    steps.append({'step': 'time scale reply', 'reply': r, 'pass': 'scaled' in r})
    send('ui do world.timescale 1')
    settle('world.timescale')
    step('free camera on', 'ui do camera.freecam 1', lambda r: r.endswith('queued'))
    r = settle('camera.freecam')
    steps.append({'step': 'free camera reply', 'reply': r, 'pass': 'still working' not in r and 'nothing sent' not in r})
    time.sleep(0.5)
    here = send('camera')
    steps.append({'step': 'camera reply (for the ParseCamera test)', 'reply': here, 'pass': here.startswith('camera pos')})
    p = [float(x) for x in here.split()[2:5]] if here.startswith('camera pos') else [0, 0, 0]
    step('save spot 1', 'ui do camera.save 1', lambda r: r.endswith('queued'))
    settle('camera.save')
    camto = []
    for k in range(1, 6):                         # 5 m up in 1 m steps (no camera jumps)
        camto.append(send('camto %.2f %.2f %.2f' % (p[0], p[1] + k, p[2])))
        time.sleep(0.1)
    steps.append({'step': 'camto replies', 'reply': ' | '.join(camto), 'pass': all(c.startswith('camera at') for c in camto)})
    time.sleep(0.3)
    away = send('camera')
    step('go to spot 1', 'ui do camera.go 1', lambda r: r.endswith('queued'))
    time.sleep(3.0)                               # ~5 steps, 60 ms apart, after the position ask
    back = send('camera')

    def dist(reply):
        if not reply.startswith('camera pos'):
            return 999.0
        q = [float(x) for x in reply.split()[2:5]]
        return sum((a - b) ** 2 for a, b in zip(p, q)) ** 0.5
    steps.append({'step': 'camera moved away then flew back', 'away_m': round(dist(away), 2),
                  'back_m': round(dist(back), 2), 'pass': dist(away) > 3.0 and dist(back) < 0.5})
    step('tab Camera', 'ui tab camera', lambda r: r.startswith('ui open tab Camera'))
    time.sleep(1.0)
    step('frame Camera tab', 'grabframe %s\\menu_camera.bmp' % out.replace('/', '\\'), lambda r: r.startswith('captured'))
    step('free camera off', 'ui do camera.freecam 0', lambda r: r.endswith('queued'))
    step('unknown control', 'ui do nope 1', lambda r: r.startswith('ui do: unknown control'))
    step('close at end', 'ui close', lambda r: r == 'ui closed')

    # ---- Task 6: Diagnostics and Advanced ----
    step('open Diagnostics', 'ui open', lambda r: r.startswith('ui open'))
    send('ui tab diagnostics')
    time.sleep(6.0)                               # it polls once a second while shown
    diag = send('ui do diag')
    ft = send('frametimes 10')
    ui_fps = float(diag.split()[2]) if diag.startswith('diag fps') else 0.0
    tcp = FrameStatsFps(ft)
    steps.append({'step': 'Diagnostics fps within 10% of frametimes 10', 'reply': diag + ' || ' + ft,
                  'pass': tcp > 0 and abs(ui_fps - tcp) <= 0.10 * tcp})
    steps.append({'step': 'Diagnostics tick rate shown', 'reply': diag, 'pass': ' tickrate ' in diag and float(diag.split()[8]) > 0})
    t1 = send('ticks')
    time.sleep(1.0)
    t2 = send('ticks')
    try:
        dt = int(t2.split()[1]) - int(t1.split()[1])
    except (IndexError, ValueError):
        dt = -1
    steps.append({'step': 'ticks 1 s apart differ by 28-32', 'reply': '%s -> %s (%d)' % (t1, t2, dt), 'pass': 28 <= dt <= 32})
    step('frame Diagnostics tab', 'grabframe %s\\menu_diagnostics.bmp' % out.replace('/', '\\'), lambda r: r.startswith('captured'))
    send('ui tab advanced')
    step('console levelname', 'ui do console levelname', lambda r: r.endswith('queued'))
    time.sleep(1.0)
    mine, theirs = send('ui do console-last'), send('levelname')
    steps.append({'step': 'console reply equals TCP reply', 'reply': mine + ' || ' + theirs, 'pass': mine == theirs and mine != ''})
    step('help lists commands', 'help ui', lambda r: r.startswith('ui open|close'))
    time.sleep(1.0)
    step('frame Advanced tab', 'grabframe %s\\menu_advanced.bmp' % out.replace('/', '\\'), lambda r: r.startswith('captured'))
    send('ui close')

    t = time.time()
    queue = send('ui do queue')
    while queue.count('idle') < 2 and time.time() - t < 15:
        time.sleep(0.5)
        queue = send('ui do queue')
    steps.append({'step': 'menu queue idle at the end', 'reply': queue, 'pass': queue.count('idle') == 2})
    if queue.count('idle') < 2:
        with open(out + '/threads.txt', 'w', encoding='utf-8') as f:
            f.write(send('threads', timeout=40))

    menu_log = log_lines('menu:') + log_lines('inputhook:')
    faults = [l for l in log_lines('') if 'fault' in l.lower() or 'exception' in l.lower()]
    result = {'pass': all(s['pass'] for s in steps), 'steps': steps, 'menu_log': menu_log[-12:],
              'fault_lines': faults[-12:], 'seconds': round(time.time() - T0, 1)}
    with open(out + '/ui_check.json', 'w', encoding='utf-8') as f:
        json.dump(result, f, indent=1)
    print('PASS' if result['pass'] else 'FAIL', flush=True)


if __name__ == '__main__':
    main()
