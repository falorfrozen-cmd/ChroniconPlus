"""Send a command to the running ChroniconPlus mod and print its reply.

    py -3 cp.py status
    py -3 cp.py "stats crit"
    py -3 cp.py "stat critc add 10"
    py -3 cp.py --wait-player          (waits until a character is loaded)

The mod polls <game>\\ChroniconPlus\\ipc\\cmd.txt from the game thread (Draw GUI in the world, obj_menu's Step in
the main menu), so the game must be running.
"""
import os
import sys
import time
import uuid

# CHRONICONPLUS_IPC points at another install's ipc folder (e.g. a test copy of the game).
IPC = os.environ.get('CHRONICONPLUS_IPC') or os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'ChroniconPlus', 'ipc')


def send(command, timeout=10.0):
    nonce = uuid.uuid4().hex
    reply_path = os.path.join(IPC, 'reply.txt')
    temporary = os.path.join(IPC, 'cmd.tmp')
    with open(temporary, 'w', encoding='utf-8', newline='\n') as out:
        out.write(nonce + '\n' + command + '\n')
    os.replace(temporary, os.path.join(IPC, 'cmd.txt'))

    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(reply_path, encoding='utf-8') as reply:
                first, _, body = reply.read().partition('\n')
            if first.strip() == nonce:
                os.remove(reply_path)
                return body
        except OSError:
            pass
        time.sleep(0.05)
    try:
        os.remove(os.path.join(IPC, 'cmd.txt'))
    except OSError:
        pass
    raise TimeoutError('no reply in %.0f s: is the game running and not paused?' % timeout)


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    if args[0] == '--wait-player':
        limit = float(args[1]) if len(args) > 1 else 1800
        start = time.time()
        while time.time() - start < limit:
            try:
                status = send('status', timeout=3)
                if 'oyuncu    yüklü' in status:
                    print(status)
                    return 0
            except (TimeoutError, OSError):
                pass
            time.sleep(3)
        print('timed out waiting for a loaded character')
        return 1
    try:
        print(send(' '.join(args)))
    except TimeoutError as error:
        print('ERR', error)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
