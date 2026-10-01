"""Real TCP peers: framing, continuous forwarding, recovery, and malformed input."""
import json
import socket
import struct
import subprocess
import sys
import tempfile
import time


def listener():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('127.0.0.1', 0))
    s.listen()
    s.settimeout(4)
    return s


def frame(payload):
    if isinstance(payload, dict):
        payload = json.dumps(payload).encode()
    return struct.pack('!I', len(payload)) + payload


def control(action, reason):
    return dict(version=1, type='control', device_id='server-wsl-01',
                message_id='original-control-' + action,
                data=dict(timestamp_ms=1790812345800, action=action, reason=reason))


def exact(s, count):
    data = b''
    while len(data) < count:
        chunk = s.recv(count - len(data))
        if not chunk:
            raise EOFError('peer closed')
        data += chunk
    return data


def read(s):
    size = struct.unpack('!I', exact(s, 4))[0]
    assert 0 < size <= 1024 * 1024
    return exact(s, size)


def until(s, predicate):
    deadline = time.monotonic() + 4
    while time.monotonic() < deadline:
        data = read(s)
        msg = json.loads(data)
        if predicate(msg):
            return data
    raise AssertionError('expected message missing')


def action_is(action, reason=None):
    return lambda m: m['type'] == 'control' and m['data']['action'] == action and (
        reason is None or m['data']['reason'] == reason)


def connect(port):
    deadline = time.monotonic() + 4
    while True:
        try:
            s = socket.create_connection(('127.0.0.1', port), timeout=2)
            s.settimeout(4)
            return s
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(.02)


def vision(i):
    # Deliberate whitespace: byte-preserving relay rather than reserialization.
    return ('{ "version":1, "type":"vision", "device_id":"jetson-01", '
            '"message_id":"vision-%d", "data":{"frame_id":%d, '
            '"timestamp_ms":1790812345678,"class_id":2,"class_name":"car",'
            '"confidence":0.91,"bbox":{"x":120,"y":210,"width":95,"height":70}}}'
            % (i, i)).encode()


def main():
    server = listener()
    reserve = listener()
    port = reserve.getsockname()[1]
    reserve.close()
    peers = []
    with tempfile.TemporaryFile(mode='w+') as log:
        p = subprocess.Popen([sys.argv[1], '127.0.0.1', str(server.getsockname()[1]),
                              str(port), '--listen-host', '127.0.0.1',
                              '--reconnect-ms', '50'],
                             stdout=log, stderr=log)
        try:
            w, _ = server.accept(); peers.append(w); w.settimeout(4)
            j = connect(port); peers.append(j)
            until(j, action_is('pause'))  # TCP up is insufficient.
            j.sendall(frame(vision(0)))
            pause = control('pause', 'database_write_failed')
            w.sendall(frame(pause))
            assert json.loads(until(j, action_is('pause', 'database_write_failed'))) == pause
            w.sendall(frame(control('resume', 'server_ready')))
            until(j, action_is('resume'))
            # Fragment both prefix and payload.
            data = frame(vision(1))
            for start in range(0, len(data), 3):
                j.sendall(data[start:start + 3])
            assert until(w, lambda m: m['type'] == 'vision') == vision(1)
            # No ACK is provided at any point.
            j.sendall(b''.join(frame(vision(i)) for i in range(2, 102)))
            for i in range(2, 102):
                assert read(w) == vision(i)
            w.sendall(frame(pause))
            until(j, action_is('pause', 'database_write_failed'))
            j.sendall(b''.join(frame(vision(i)) for i in range(200, 220)))
            time.sleep(.15)
            resume = control('resume', 'database_recovered')
            w.sendall(frame(resume))
            until(j, action_is('resume'))
            j.sendall(frame(vision(300)))
            assert until(w, lambda m: m['type'] == 'vision') == vision(300)
            j.close()
            until(w, action_is('pause', 'jetson_connection_lost'))
            j = connect(port); peers.append(j)
            until(j, action_is('resume'))
            until(w, action_is('resume', 'jetson_connection_restored'))
            w.close()
            until(j, action_is('pause', 'wsl_connection_lost'))
            w, _ = server.accept(); peers.append(w); w.settimeout(4)
            w.sendall(frame(control('pause', 'database_write_failed')))
            until(j, action_is('pause', 'database_write_failed'))
            # A Jetson reconnect while downstream paused must stay paused.
            j.close()
            until(w, action_is('pause', 'jetson_connection_lost'))
            j = connect(port); peers.append(j)
            until(j, action_is('pause'))
            w.sendall(frame(resume))
            until(j, action_is('resume'))
            j.sendall(frame(vision(400)))
            assert until(w, lambda m: m['type'] == 'vision') == vision(400)
            # Invalid JSON closes only the offender, process keeps recovering.
            j.sendall(frame(b'{"version":1,}'))
            until(w, action_is('pause', 'jetson_connection_lost'))
            j.close()
            j = connect(port); peers.append(j)
            until(j, action_is('resume'))
            j.sendall(struct.pack('!I', 1024 * 1024 + 1))
            until(w, action_is('pause', 'jetson_connection_lost'))
            assert p.poll() is None
            p.terminate()
            assert p.wait(timeout=3) == 0
            print('PASS: framing, 101 continuous messages, byte preservation, pause discard, '
                  'both link recoveries, state sync, malformed JSON, length cap, shutdown')
        except BaseException:
            log.seek(0)
            print(log.read(), file=sys.stderr)
            raise
        finally:
            for peer in peers:
                peer.close()
            server.close()
            if p.poll() is None:
                p.kill(); p.wait()


if __name__ == '__main__':
    main()
