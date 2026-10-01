"""Bounded queues, pending sessions, partial frames, and backpressure recovery."""
import contextlib
import json
import re
import socket
import subprocess
import sys
import tempfile
import time

from integration import (action_is, connect, control, frame, listener, read,
                         until, vision)


class Gateway:
    def __init__(self, *options):
        self.server = listener()
        reserve = listener()
        self.port = reserve.getsockname()[1]
        reserve.close()
        self.log = tempfile.TemporaryFile(mode='w+')
        self.peers = []
        self.process = subprocess.Popen(
            [sys.argv[1], '--listen-host', '127.0.0.1', '--listen-port', str(self.port),
             '--wsl-port', str(self.server.getsockname()[1]), '--reconnect-ms', '50',
             *options], stdout=self.log, stderr=self.log)

    def upstream(self):
        j = connect(self.port)
        self.peers.append(j)
        return j

    def downstream(self):
        w, _ = self.server.accept()
        w.settimeout(4)
        self.peers.append(w)
        return w

    def ready(self):
        w = self.downstream()
        w.sendall(frame(control('resume', 'server_ready')))
        j = self.upstream()
        until(j, action_is('pause'))
        until(j, action_is('resume'))
        # Consume the upstream restoration report before raw Vision reads.
        until(w, action_is('resume', 'jetson_connection_restored'))
        return j, w

    def logs(self):
        self.log.seek(0)
        return self.log.read()

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            assert self.process.wait(timeout=3) == 0
        for peer in self.peers:
            peer.close()
        self.server.close()
        self.log.close()


@contextlib.contextmanager
def gateway(*options):
    g = Gateway(*options)
    try:
        yield g
    except BaseException:
        print(g.logs(), file=sys.stderr)
        raise
    finally:
        g.close()


def pause_partial_input():
    with gateway() as g:
        j, w = g.ready()
        pause = control('pause', 'database_write_failed')
        w.sendall(frame(pause))
        assert json.loads(until(j, action_is('pause', 'database_write_failed'))) == pause
        old = frame(vision(10))
        j.sendall(old[:9])  # Frame started while paused, completed after resume.
        time.sleep(.1)
        resume = control('resume', 'database_recovered')
        w.sendall(frame(resume))
        assert json.loads(until(j, action_is('resume'))) == resume
        j.sendall(old[9:] + frame(vision(11)))
        assert until(w, lambda m: m['type'] == 'vision') == vision(11)
        # Generated synchronization safely quotes arbitrary WSL reason strings.
        pause = control('pause', 'database "write" failed\n경고')
        w.sendall(frame(pause))
        assert json.loads(until(j, action_is('pause', pause['data']['reason']))) == pause
        j.close()
        until(w, action_is('pause', 'jetson_connection_lost'))
        j = g.upstream()
        assert json.loads(until(j, action_is('pause')))['data']['reason'] == pause['data']['reason']
        # Another socket must not replace the active producer.
        extra = g.upstream()
        assert extra.recv(1) == b''


def queue_overflow():
    with gateway('--queue-capacity', '2') as g:
        j, w = g.ready()
        j.sendall(b''.join(frame(vision(i)) for i in range(1000)))
        # Read all accepted messages without sending any application ACK.
        w.settimeout(.2)
        count = 0
        while True:
            try:
                msg = json.loads(read(w))
                assert msg['type'] == 'vision'
                count += 1
            except socket.timeout:
                break
        assert 0 < count < 1000
        w.settimeout(4)
        j.sendall(frame(vision(9999)))
        assert read(w) == vision(9999)
        time.sleep(1.05)
        logs = g.logs()
        overflows = [int(n) for n in re.findall(r'overflow_dropped=(\d+)', logs)]
        assert overflows and max(overflows) > 0
        assert all(int(n) <= 2 for n in re.findall(r'queue=(\d+)', logs))
        received = re.findall(r'received=(\d+)', logs)
        forwarded = re.findall(r'forwarded=(\d+)', logs)
        assert int(received[-1]) == 1001
        assert int(forwarded[-1]) == count + 1


def partial_output_pause():
    with gateway('--send-timeout-ms', '3000') as g:
        # Reduce receive window before accepting to force partial TCP writes.
        g.server.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        j, w = g.ready()
        big = json.loads(vision(1))
        big['data']['padding'] = 'x' * 300000
        batch = frame(big)
        for _ in range(100):
            j.sendall(batch)
        time.sleep(.1)
        pause = control('pause', 'database_write_failed')
        w.sendall(frame(pause))
        assert json.loads(until(j, action_is('pause', 'database_write_failed'))) == pause
        # A partly sent Vision causes a clean session reset; no continuation is
        # sent on that session and the old backlog never reaches the new peer.
        until(j, action_is('pause', 'wsl_connection_lost'))
        new_w = g.downstream()
        new_w.sendall(frame(control('resume', 'server_ready')))
        until(j, action_is('resume', 'server_ready'))
        j.sendall(frame(vision(7777)))
        assert until(new_w, lambda m: m['type'] == 'vision') == vision(7777)
        time.sleep(1.05)
        assert any(int(n) > 0 for n in re.findall(r'discarded_on_pause=(\d+)', g.logs()))


def initial_downstream_down():
    with gateway() as g:
        port = g.server.getsockname()[1]
        g.server.close()
        j = g.upstream()
        until(j, action_is('pause', 'wsl_connection_lost'))
        j.sendall(frame(vision(1)))
        time.sleep(.15)
        server = socket.socket()
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(('127.0.0.1', port)); server.listen(); server.settimeout(4)
        g.server = server
        w = g.downstream()
        w.sendall(frame(control('resume', 'server_ready')))
        until(j, action_is('resume', 'server_ready'))
        j.sendall(frame(vision(2)))
        assert until(w, lambda m: m['type'] == 'vision') == vision(2)


def deferred_resume_and_buffered_input():
    with gateway() as g:
        w = g.downstream()
        resume = control('resume', 'server_ready')
        # WSL reports recovery while Jetson is disconnected.
        w.sendall(frame(resume))
        time.sleep(.1)
        j = g.upstream()
        assert json.loads(until(j, action_is('resume'))) == resume
        until(w, action_is('resume', 'jetson_connection_restored'))
        pause = control('pause', 'queue_overload')
        w.sendall(frame(pause))
        assert json.loads(until(j, action_is('pause', 'queue_overload'))) == pause
        # More than one RX work budget: all old bytes must drain before RESUME.
        j.sendall(b''.join(frame(vision(i)) for i in range(1000)))
        w.sendall(frame(resume))
        assert json.loads(until(j, action_is('resume'))) == resume
        j.sendall(frame(vision(12345)))
        assert until(w, lambda m: m['type'] == 'vision') == vision(12345)


def control_passthrough_and_load():
    with gateway() as g:
        j, w = g.ready()
        health = control('health', 'periodic_check')
        w.sendall(frame(health))
        assert json.loads(read(j)) == health
        upstream_control = control('health', 'jetson_periodic_check')
        j.sendall(frame(upstream_control))
        assert json.loads(read(w)) == upstream_control
        count = 5000
        start = time.monotonic()
        j.sendall(b''.join(frame(vision(i)) for i in range(count)))
        for i in range(count):
            assert read(w) == vision(i)
        elapsed = time.monotonic() - start
        time.sleep(1.05)
        logs = g.logs()
        assert 'received=5000 forwarded=5000 queue=0' in logs
        assert 'overflow_dropped=0' in logs
        print(f'continuous load: {count} messages, {elapsed:.3f}s, {count / elapsed:.0f} msg/s')


def send_stall_recovery():
    with gateway('--send-timeout-ms', '500') as g:
        g.server.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        j, w = g.ready()
        big = json.loads(vision(1))
        big['data']['padding'] = 'x' * 300000
        payload = frame(big)
        for _ in range(100):
            j.sendall(payload)
        until(j, action_is('pause', 'wsl_connection_lost'))
        new_w = g.downstream()
        new_w.sendall(frame(control('resume', 'server_ready')))
        until(j, action_is('resume', 'server_ready'))
        j.sendall(frame(vision(8888)))
        assert until(new_w, lambda m: m['type'] == 'vision') == vision(8888)


def zero_and_duplicate_input():
    for bad in [b'', b'{"version":1,"version":1}', b'{}\0', b'"\xc0\xaf"']:
        with gateway() as g:
            j, w = g.ready()
            j.sendall(frame(bad))
            until(w, action_is('pause', 'jetson_connection_lost'))
            assert g.process.poll() is None


if __name__ == '__main__':
    for test in [pause_partial_input, queue_overflow, partial_output_pause,
                 initial_downstream_down, zero_and_duplicate_input,
                 control_passthrough_and_load, send_stall_recovery,
                 deferred_resume_and_buffered_input]:
        test()
        print('PASS:', test.__name__)
