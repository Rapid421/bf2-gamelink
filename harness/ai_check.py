"""In-game check of the AI build (build.bat ai): command port works, no menu, nothing drawn or hooked unasked.

    python -I ai_check.py <out-dir>

Writes <out-dir>/ai_check.json with every reply and a pass/fail per step. Hard limit 200 s.
"""
import json
import socket
import sys
import time

LOG = 'C:/ProgramData/FrostyGameLink/gamelink.log'


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
            d = s.recv(262144)
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


def this_run_log():
    found = []
    try:
        with open(LOG, encoding='utf-8', errors='replace') as f:
            for line in f:
                if 'FrostyGameLink attached' in line:
                    found = []
                found.append(line.rstrip())
    except OSError:
        pass
    return found


def main():
    out = sys.argv[1]
    if len(out) > 2 and out[0] == '/' and out[2] == '/':
        out = out[1].upper() + ':' + out[2:]
    steps = []

    def step(name, cmd, ok):
        reply = send(cmd)
        steps.append({'step': name, 'cmd': cmd, 'reply': reply[:400], 'pass': bool(ok(reply))})
        print(json.dumps(steps[-1]), flush=True)
        return reply

    step('ping', 'ping', lambda r: r == 'pong gamelink')
    step('level', 'levelname', lambda r: r.startswith('loaded level:'))
    step('help lists every command', 'help', lambda r: r.count('\n') >= 120)
    step('no menu', 'ui open', lambda r: 'AI build' in r and 'no menu' in r)
    step('camera', 'camera', lambda r: r.startswith('camera pos'))
    step('reflection', 'types', lambda r: r.startswith('types '))
    step('bots status', 'bots status', lambda r: r.startswith('bots '))
    step('ticks', 'ticks', lambda r: r.startswith('ticks '))
    step('frame capture', 'grabframe %s\\ai_frame.bmp' % out.replace('/', '\\'), lambda r: r.startswith('captured'))
    log = this_run_log()
    menu_lines = [l for l in log if 'menu:' in l]
    steps.append({'step': 'nothing menu-related ran', 'reply': ' | '.join(menu_lines[:5]), 'pass': not menu_lines})
    steps.append({'step': 'log says AI build', 'reply': next((l for l in log if 'AI build' in l), ''),
                  'pass': any('AI build' in l for l in log)})
    result = {'pass': all(s['pass'] for s in steps), 'steps': steps}
    with open(out + '/ai_check.json', 'w', encoding='utf-8') as f:
        json.dump(result, f, indent=1)
    print('PASS' if result['pass'] else 'FAIL', flush=True)


if __name__ == '__main__':
    main()
