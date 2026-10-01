"""Final WSL contract: unsolicited first state, reconnect reset, event separation."""
import json
import socket
import time

from integration import action_is, control, frame, read, until, vision
from recovery import gateway


def available(peer):
    peer.settimeout(.15)
    result = []
    try:
        while True:
            result.append(json.loads(read(peer)))
    except socket.timeout:
        return result
    finally:
        peer.settimeout(4)


def assert_no_resume(peer):
    assert all(not action_is('resume')(m) for m in available(peer))


def assert_no_vision_or_request(peer):
    messages = available(peer)
    assert all(m['type'] != 'vision' and m['data']['action'] != 'state_request'
               for m in messages)


def ready_pause_reconnect():
    with gateway() as g:
        w = g.downstream()
        ready = control('resume', 'server_ready')
        w.sendall(frame(ready))  # First frame, without reading or waiting for a request.
        j = g.upstream()
        assert json.loads(until(j, action_is('resume'))) == ready
        until(w, action_is('resume', 'jetson_connection_restored'))
        assert_no_vision_or_request(w)
        j.sendall(frame(vision(1)))
        assert read(w) == vision(1)
        w.close()
        until(j, action_is('pause', 'wsl_connection_lost'))

        w = g.downstream()
        paused = control('pause', 'database_write_failed')
        w.sendall(frame(paused))  # First frame on the new session is PAUSED.
        assert json.loads(until(j, action_is('pause', 'database_write_failed'))) == paused
        # Restoration event cannot undo DB failure, even with action=resume.
        w.sendall(frame(control('resume', 'pi_connection_restored')))
        assert_no_resume(j)
        j.sendall(frame(vision(2)))
        assert_no_vision_or_request(w)
        # Duplicate status reports do not create false state transitions.
        w.sendall(frame(paused) + frame(paused))
        assert_no_resume(j)
        recovered = control('resume', 'database_recovered')
        w.sendall(frame(recovered))
        assert json.loads(until(j, action_is('resume'))) == recovered
        until(w, action_is('resume', 'jetson_connection_restored'))
        w.sendall(frame(recovered) + frame(recovered))
        assert json.loads(read(j)) == recovered
        assert json.loads(read(j)) == recovered
        # Same event with action=pause must not change READY either.
        w.sendall(frame(control('pause', 'pi_connection_restored')))
        time.sleep(.05)
        j.sendall(frame(vision(3)))
        assert read(w) == vision(3)
        assert g.logs().count('state=RUNNING reason=none') == 2
        w.close()
        until(j, action_is('pause', 'wsl_connection_lost'))

        w = g.downstream()
        # Old session readiness is never reused; unrelated RESUMEs cannot
        # complete the new session's initial internal status synchronization.
        w.sendall(frame(control('resume', 'pi_connection_restored')))
        w.sendall(frame(control('resume', 'database_recovered')))
        assert_no_resume(j)
        j.sendall(frame(vision(4)))
        assert_no_vision_or_request(w)
        w.sendall(frame(ready))
        assert json.loads(until(j, action_is('resume'))) == ready
        until(w, action_is('resume', 'jetson_connection_restored'))
        j.sendall(frame(vision(5)))
        assert read(w) == vision(5)


def initial_paused():
    with gateway() as g:
        w = g.downstream()
        paused = control('pause', 'queue_overload')
        w.sendall(frame(paused))
        time.sleep(.05)
        j = g.upstream()
        until(j, action_is('pause', 'queue_overload'))
        assert_no_resume(j)
        j.sendall(frame(vision(6)))
        assert_no_vision_or_request(w)
        resumed = control('resume', 'queue_recovered')
        w.sendall(frame(resumed))
        assert json.loads(until(j, action_is('resume'))) == resumed
        until(w, action_is('resume', 'jetson_connection_restored'))
        j.sendall(frame(vision(7)))
        assert read(w) == vision(7)


if __name__ == '__main__':
    ready_pause_reconnect()
    initial_paused()
    print('PASS: unsolicited first READY/PAUSED, all reconnects, no state_request, '
          'connection event separation, duplicate statuses, original Control preservation')
