# 실제 Gateway 검증 결과

실행: `./build/pi_gateway 10.10.16.3 9000 8000`

Pi: `10.10.16.243:8000` / WSL: `10.10.16.3:9000` / Jetson: `10.10.16.153`

Gateway PID: `20388` (`gateway.pid`). Gateway는 백그라운드에서 계속 실행합니다.
상태·metrics 로그: `logs/live-validation.log`, 실제 socket syscall 증거: `logs/live-wire.trace`.

## 정상 실시간 전달

최초 WSL 연결 장애 직전 관측:

```text
metrics rx_per_s=36.7 tx_per_s=36.7 received=1796 forwarded=1796 queue=0 queue_bytes=0 overflow_dropped=0 discarded_on_pause=0 jetson=UP wsl=UP wsl_internal=READY pi=RUNNING reason="none" jetson_reconnects=0 wsl_reconnects=0
```

Received=Forwarded, queue=0, overflow=0을 실제 Jetson/Pi/WSL 연결에서 확인했습니다.
Forwarded는 전체 프레임을 로컬 TCP 송신 버퍼에 전달한 수이며 WSL DB 저장 완료 확인은 아닙니다.

## WSL 연결 장애와 Jetson PAUSE

```text
metrics rx_per_s=9.4 tx_per_s=9.4 received=1806 forwarded=1806 queue=0 queue_bytes=0 overflow_dropped=0 discarded_on_pause=0 jetson=UP wsl=DOWN wsl_internal=UNKNOWN pi=PAUSED reason="wsl_connection_lost" jetson_reconnects=0 wsl_reconnects=0
20391 sendto(5, "\0\0\0\316{\"version\":1,\"type\":\"control\",\"device_id\":\"gateway-pi-01\",\"message_id\":\"gateway-pi-01-1790826626374-20388-000000000005\",\"data\":{\"timestamp_ms\":1790826716141,\"action\":\"pause\",\"reason\":\"wsl_connection_lost\"}}", 210, MSG_NOSIGNAL, NULL, 0) = 210
```

WSL 연결 종료를 직접 관측하여 Pi PAUSE, 큐 비우기, Jetson socket(fd 5)에
`pause/wsl_connection_lost` 프레임 210바이트 전체 송신을 확인했습니다.
상대 Jetson 애플리케이션 내부 처리 완료까지 증명하는 ACK는 없으므로 송신 성공까지의 증거입니다.
장비 전원 종료나 프로세스 종료의 실제 원인을 socket 관측만으로 단정하지 않습니다.

## 복구 시 첫 세션 상태 Control을 받은 뒤 재개

TCP 연결 복구 직후에도 UNKNOWN/PAUSED를 유지:

```text
metrics rx_per_s=0.0 tx_per_s=0.0 received=1806 forwarded=1806 queue=0 queue_bytes=0 overflow_dropped=0 discarded_on_pause=0 jetson=UP wsl=UP wsl_internal=UNKNOWN pi=PAUSED reason="wsl_state_pending" jetson_reconnects=0 wsl_reconnects=1
```

WSL 새 session에서 첫 payload로 `resume/server_ready` 수신(180바이트),
원본 message_id를 유지하여 Jetson에 RESUME 전체 송신(184바이트):

```text
20390 recvfrom(4, "{\"version\":1,\"type\":\"control\",\"device_id\":\"server-wsl-01\",\"message_id\":\"server-wsl-01-000000000002\",\"data\":{\"timestamp_ms\":1790826718162,\"action\":\"resume\",\"reason\":\"server_ready\"}}", 180, 0, NULL, NULL) = 180
20391 sendto(5, "\0\0\0\264{\"version\":1,\"type\":\"control\",\"device_id\":\"server-wsl-01\",\"message_id\":\"server-wsl-01-000000000002\",\"data\":{\"timestamp_ms\":1790826718162,\"action\":\"resume\",\"reason\":\"server_ready\"}}", 184, MSG_NOSIGNAL, NULL, 0) = 184
```

그 뒤 READY/RUNNING과 새 Vision 전달 재개:

```text
metrics rx_per_s=8.9 tx_per_s=8.9 received=1815 forwarded=1815 queue=0 queue_bytes=0 overflow_dropped=0 discarded_on_pause=0 jetson=UP wsl=UP wsl_internal=READY pi=RUNNING reason="none" jetson_reconnects=0 wsl_reconnects=1
```

이후에도 첫 상태 Control이 없는 TCP 연결들이 반복해서 관측되었습니다.
그 세션에서는 UNKNOWN/PAUSED를 유지하고, TCP 연결 성공만으로 RUNNING에 들어가지 않았습니다.

## 기록 시점의 최신 상태

```text
metrics rx_per_s=29.9 tx_per_s=30.9 received=2328 forwarded=2328 queue=0 queue_bytes=0 overflow_dropped=0 discarded_on_pause=0 jetson=UP wsl=UP wsl_internal=READY pi=RUNNING reason="none" jetson_reconnects=0 wsl_reconnects=25
```

최신 상태가 PAUSED/UNKNOWN이면 WSL이 현재 세션의 첫 `resume/server_ready` 또는
`pause/<실제 장애 reason>`을 전달해야 합니다. Pi 재접속과 Control 경로는 계속 실행됩니다.

## 관측 event 시각 (Asia/Seoul)

```text
2026-10-01T12:50:26.402+09:00 event=wsl_connection_restored
2026-10-01T12:50:35.994+09:00 event=jetson_connection_restored
2026-10-01T12:51:56.141+09:00 event=wsl_connection_lost
2026-10-01T12:51:57.152+09:00 event=wsl_connection_restored
2026-10-01T12:52:02.417+09:00 event=wsl_connection_lost
2026-10-01T12:52:03.440+09:00 event=wsl_connection_restored
2026-10-01T12:52:05.490+09:00 event=wsl_connection_lost
2026-10-01T12:52:06.535+09:00 event=wsl_connection_restored
```
