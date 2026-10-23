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
        with open(LOG, encoding='utf-8', errors='replace') as f:
            lines = f.read().splitlines()
    except OSError:
        return []
    start = max((i for i, l in enumerate(lines) if 'FrostyGameLink attached' in l), default=0)   # this run only
    return [l for l in lines[start:] if needle in l]


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
    step('starts closed', 'ui status', lambda r: r == 'ui closed')
    step('opens', 'ui open', lambda r: r.startswith('ui open tab Camera input blocked'))
    step('tab by name', 'ui tab bots', lambda r: r.startswith('ui open tab Bots'))
    time.sleep(1.5)                               # let a few frames draw the menu
    step('frame with menu', 'grabframe %s\\menu_open.bmp' % out.replace('/', '\\'), lambda r: r.startswith('captured'))
    step('focus loss closes', 'ui test-focuslost', lambda r: r == 'ui closed')
    step('toggle reopens on the same tab', 'ui toggle', lambda r: r.startswith('ui open tab Bots'))
    step('close', 'ui close', lambda r: r == 'ui closed')
    step('bad tab rejected', 'ui tab nope', lambda r: r.startswith('ui: unknown tab'))
    time.sleep(1.0)
    step('frame closed (badge)', 'grabframe %s\\menu_closed.bmp' % out.replace('/', '\\'), lambda r: r.startswith('captured'))

    menu_log = log_lines('menu:') + log_lines('inputhook:')
    faults = [l for l in log_lines('') if 'fault' in l.lower() or 'exception' in l.lower()]
    result = {'pass': all(s['pass'] for s in steps), 'steps': steps, 'menu_log': menu_log[-12:],
              'fault_lines': faults[-12:], 'seconds': round(time.time() - T0, 1)}
    with open(out + '/ui_check.json', 'w', encoding='utf-8') as f:
        json.dump(result, f, indent=1)
    print('PASS' if result['pass'] else 'FAIL', flush=True)


if __name__ == '__main__':
    main()
