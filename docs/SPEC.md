# Raspberry Pi Gateway 개발 명세 — 최종본

## 1. 목적

Raspberry Pi Gateway는 Jetson Vision Client와 WSL Final Server 사이의 **실시간 TCP Gateway**다.

전체 구조:

```text
Jetson Vision Client
        ↕
Raspberry Pi Gateway
        ↕
WSL Final Server
```

Pi의 핵심 책임:

```text
Jetson에서 vision 수신
→ WSL로 실시간 Forward

WSL에서 control 수신
→ 필요 시 Jetson으로 전달

Jetson ↔ Pi 연결 상태 직접 판단
Pi ↔ WSL 연결 상태 직접 판단

한쪽 Link에서 발생한 장애를
살아 있는 반대쪽 Link로 Control 전파

장애 시 Data Path PAUSE
복구 시 현재 시점의 새로운 데이터부터 RESUME
```

Pi는 다음 기능을 담당하지 않는다.

```text
YOLO inference
Camera
Tracking
차량 Count
최종 DB 저장
Vision 통계 분석
```

---

# 2. 최상위 Network / Control 계약

전체 시스템의 핵심 원칙:

```text
1. 각 노드는 자신이 직접 관찰 가능한 장애를 스스로 판단한다.

2. 자신이 직접 관찰할 수 없는 다른 구간의 상태만
   Control 메시지로 전달받는다.

3. 다른 노드가 알아야 하는 상태는
   살아 있는 Link를 통해 Control로 전파한다.

4. 정상 vision은
   Jetson → Pi,
   Pi → WSL
   두 구간 모두 continuous send 한다.

5. Vision마다 application-level ACK를 사용하지 않는다.

6. 장애 중 Vision backlog를 만들지 않는다.

7. PAUSE 시 기존 대기 Vision도 폐기한다.

8. RESUME 후 현재 시점의 새로운 Vision부터 다시 처리한다.
```

---

# 3. 전체 통신 구조

Pi는 두 개의 독립적인 TCP Link를 관리한다.

```text
Link A
Jetson ↔ Pi

Link B
Pi ↔ WSL
```

각 Link는 독립적으로 상태를 가진다.

최소 상태:

```text
jetson_link = UP / DOWN

wsl_link = UP / DOWN

wsl_internal_paused = true / false

pi_state = RUNNING / PAUSED
```

---

# 4. 정상 Vision 흐름

정상 상태:

```text
Jetson
  │
  │ vision
  ▼
 Pi
  │
  │ same vision
  ▼
WSL Final Server
```

Pi는 Jetson의 Vision payload 의미를 변경하지 않는다.

유지해야 하는 값:

```text
version
type
device_id
message_id

frame_id
timestamp_ms

class_id
class_name
confidence

bbox
```

Pi는 새로운 Detection ID나 새로운 Vision message_id를 생성하지 않는다.

---

# 5. TCP 규격

모든 구간은 TCP를 사용한다.

Framing:

```text
4-byte big-endian payload length
+
JSON payload
```

반드시 Partial Read / Partial Write를 처리한다.

다음을 가정하지 않는다.

```text
send() 한 번 = 전체 메시지 송신
recv() 한 번 = 전체 메시지 수신
```

각 Link는 IP + Port 기반이며 같은 Wi-Fi/LAN에 있을 것을 프로토콜상 전제로 하지 않는다.

---

# 6. Vision ACK 정책

기존 Stop-and-Wait 방식은 사용하지 않는다.

기존:

```text
vision 1
→ ACK
→ vision 2
→ ACK
→ vision 3
```

최종:

```text
Jetson → Pi

vision
vision
vision
vision
...
```

그리고:

```text
Pi → WSL

vision
vision
vision
vision
...
```

정상 Vision에는 application-level ACK가 없다.

TCP 자체의 신뢰성은 그대로 사용한다.

---

# 7. JSON 공통 Envelope

Vision과 Control 모두 동일한 외부 구조를 사용한다.

```json
{
  "version": 1,
  "type": "...",
  "device_id": "...",
  "message_id": "...",
  "data": {
  }
}
```

공통 필드:

```text
version
type
device_id
message_id
data
```

현재 주요 type:

```text
vision
control
```

---

# 8. Vision JSON

예:

```json
{
  "version": 1,
  "type": "vision",
  "device_id": "vision-pi-01",
  "message_id": "vision-pi-01-000059-00000001",
  "data": {
    "frame_id": 1234,
    "timestamp_ms": 1790812345678,
    "class_id": 2,
    "class_name": "car",
    "confidence": 0.91,
    "bbox": {
      "x": 120,
      "y": 210,
      "width": 95,
      "height": 70
    }
  }
}
```

대상 클래스:

```text
2 car
3 motorcycle
5 bus
7 truck
```

Pi는 해당 JSON을 WSL에 그대로 Forward한다.

---

# 9. Control JSON

Control 역시 같은 envelope를 사용한다.

예: Pi가 WSL Link 장애를 Jetson에 알림.

```json
{
  "version": 1,
  "type": "control",
  "device_id": "gateway-pi-01",
  "message_id": "gateway-pi-01-000001-00000042",
  "data": {
    "timestamp_ms": 1790812345800,
    "action": "pause",
    "reason": "wsl_connection_lost"
  }
}
```

복구:

```json
{
  "version": 1,
  "type": "control",
  "device_id": "gateway-pi-01",
  "message_id": "gateway-pi-01-000001-00000043",
  "data": {
    "timestamp_ms": 1790812349100,
    "action": "resume",
    "reason": "wsl_connection_restored"
  }
}
```

Control 역시 동일한:

```text
4-byte big-endian length-prefix + JSON
```

방식을 사용한다.

---

# 10. 장애 reason 계약

TCP 연결 장애만으로:

```text
장비 전원 OFF
프로세스 crash
Wi-Fi 장애
라우팅 문제
물리 네트워크 장애
```

중 무엇인지 확정하지 않는다.

따라서 **관찰 사실 중심 reason**을 사용한다.

Jetson Link:

```text
jetson_connection_lost
jetson_connection_restored
```

WSL Link:

```text
wsl_connection_lost
wsl_connection_restored
```

WSL 내부 장애는 WSL이 직접 원인을 알 수 있으므로 구체적인 reason 사용 가능:

```text
database_write_failed
database_recovered

server_processing_failed
server_processing_recovered

queue_overload
queue_recovered
```

최상위 원칙:

```text
직접 관찰한 사실
→ 확정적으로 기록

관찰하지 못한 원인
→ 추측하지 않음
```

---

# 11. Jetson ↔ Pi 장애

이 Link는 Jetson과 Pi가 모두 직접 관찰 가능하다.

따라서 양쪽이 각각 스스로 판단한다.

Pi 측:

```text
Jetson socket 종료
send/recv error
connection failure
```

발생:

```text
jetson_link = DOWN

→ Pi PAUSE
→ Forward Queue CLEAR
→ 신규 Vision enqueue 금지

WSL Link가 살아 있으면
→ control(PAUSE)
→ reason = jetson_connection_lost
→ WSL 전달
```

Pi는 이를:

```text
Jetson 장비가 죽었다
```

라고 단정하지 않는다.

관측한 사실은:

```text
Jetson ↔ Pi connection lost
```

이다.

---

# 12. Jetson Link 복구

Jetson이 다시 연결되면:

```text
jetson_link = UP
```

Pi는 복구 사실을 관측한다.

WSL Link가 살아 있으면:

```text
control
action = resume
reason = jetson_connection_restored
```

정보를 WSL에 전달할 수 있다.

단, Jetson 연결 하나만 복구됐다고 전체 Data Path를 즉시 RUNNING으로 만들지는 않는다.

전체 RUNNING 조건을 다시 평가한다.

---

# 13. Pi ↔ WSL 장애

Pi와 WSL 모두 이 Link를 직접 관찰한다.

Pi 측에서:

```text
WSL socket 종료
send/recv error
connect 실패
connection failure
```

발생:

```text
wsl_link = DOWN

→ Pi PAUSE
→ Forward Queue CLEAR
→ 신규 Vision enqueue 금지

Jetson Link가 살아 있으면
→ control(PAUSE)
→ reason = wsl_connection_lost
→ Jetson 전달
```

WSL도 자기 쪽에서 같은 Link 장애를 직접 감지한다.

---

# 14. WSL Link 복구

Pi가 WSL 연결 복구를 직접 확인하면:

```text
wsl_link = UP
```

으로 변경한다.

하지만:

```text
WSL TCP 연결 성공
=
WSL 내부 상태 정상
```

으로 간주하지 않는다.

WSL 내부 DB/Processing 상태는 WSL만 판단한다.

따라서 WSL 상태 동기화 후 전체 RUNNING 조건을 평가한다.

---

# 15. WSL 내부 장애

다음은 Pi가 직접 판단할 수 없는 영역이다.

```text
SQLite write 실패

SQLite 사용 불가

WSL DB Writer 처리 불능

WSL 내부 Queue 문제

WSL Server 내부 처리 실패
```

WSL이 직접 판단한다.

WSL:

```text
자체 PAUSE

→ control(PAUSE)
→ Pi
```

예:

```json
{
  "version": 1,
  "type": "control",
  "device_id": "server-wsl-01",
  "message_id": "...",
  "data": {
    "timestamp_ms": 1790812345800,
    "action": "pause",
    "reason": "database_write_failed"
  }
}
```

Pi 수신:

```text
wsl_internal_paused = true

→ Pi PAUSE
→ Forward Queue CLEAR
→ 신규 Vision enqueue 금지

→ 같은 Control을 Jetson에 전달
```

즉:

```text
WSL internal failure
→ WSL
→ Pi
→ Jetson
```

---

# 16. WSL 내부 복구

WSL 내부 복구 여부 역시 WSL이 판단한다.

예:

```text
database_recovered
server_processing_recovered
queue_recovered
```

WSL:

```text
control(RESUME)
→ Pi
```

Pi:

```text
wsl_internal_paused = false
```

그 후 전체 RUNNING 조건을 평가한다.

조건이 모두 정상일 때만 Data Path를 재개한다.

---

# 17. Pi 자체 DOWN

Pi가 죽으면 Pi는 Control을 보낼 수 없다.

그 경우:

```text
Jetson
→ Pi connection lost 직접 감지
→ 자체 PAUSE

WSL
→ Pi connection lost 직접 감지
→ 자체 PAUSE
```

따라서 시스템은 Pi가 장애 발생 전에 PAUSE 메시지를 반드시 보내야만 안전한 구조가 아니다.

각 노드는 직접 연결 상대의 장애를 스스로 판단해야 한다.

---

# 18. Jetson 자체 DOWN

Jetson 자체 DOWN을 Pi가 직접 확정하는 것이 아니다.

Pi가 실제로 아는 것은:

```text
Jetson connection lost
```

이다.

따라서:

```text
Jetson connection lost
→ Pi 자체 PAUSE
→ Queue CLEAR
→ WSL에 Control 전달
```

reason:

```text
jetson_connection_lost
```

WSL은 해당 정보를 system event로 저장해 Vision 데이터 공백의 원인으로 활용한다.

---

# 19. WSL 자체 DOWN

WSL이 완전히 종료되면 WSL은 Control을 보낼 수 없다.

Pi:

```text
WSL connection lost 직접 감지
→ wsl_link = DOWN
→ Pi PAUSE
→ Queue CLEAR

Jetson Link가 살아 있으면
→ PAUSE 전달
```

reason:

```text
wsl_connection_lost
```

---

# 20. PAUSE 의미

PAUSE는 프로그램 종료가 아니다.

```text
RUNNING
→ PAUSE
→ PAUSED
→ RESUME
→ RUNNING
```

Pi가 PAUSE 상태가 되면:

```text
Forward Queue CLEAR

신규 Vision enqueue 금지

Vision Forward 중단

Data Worker WAIT
```

하지만 다음 기능은 계속 살아 있어야 한다.

```text
Jetson Connection 관리

WSL Connection 관리

Control RX

Control TX

Reconnect

Health / State 처리
```

---

# 21. Vision Backlog 정책

본 시스템은 과거 데이터 복원이 아니라 **실시간 관제**가 목적이다.

따라서:

```text
PAUSE 발생
→ 기존 Forward Queue Vision 전부 폐기
```

PAUSED 상태:

```text
새 Vision이 들어와도
→ Queue에 쌓지 않음
→ 폐기
```

RESUME:

```text
복구 시점 이후의 새로운 Vision부터 Forward
```

장애 시간 동안 발생한 Vision을 복구 후 WSL에 보내지 않는다.

---

# 22. Forward Queue 역할

Queue는:

```text
Jetson RX
→ Forward Queue
→ WSL TX
```

사이의 짧은 처리 시간 차이를 흡수하는 실시간 Buffer다.

Persistent Storage가 아니다.

PAUSE:

```text
Queue CLEAR
```

PAUSED:

```text
Vision push X
```

---

# 23. Queue Full 정책

정상 상태에서도 순간적으로 Queue가 증가할 수 있다.

Queue Full이 발생하면 Metrics로 기록한다.

하지만:

```text
Queue 크기만 증가시켜서
병목을 숨기는 방식
```

은 사용하지 않는다.

확인할 값:

```text
Jetson received vision msg/s

WSL forwarded vision msg/s

Forward Queue depth

Overflow dropped
```

정상 상태에서 장기적으로:

```text
received msg/s ≈ forwarded msg/s
```

여야 한다.

---

# 24. Pi RUNNING 조건

Pi가 Vision을 WSL로 정상 Forward할 수 있는 조건:

```text
jetson_link == UP

AND

wsl_link == UP

AND

wsl_internal_paused == false

AND

Pi Data Path 정상
```

모든 조건이 만족돼야 한다.

하나라도 false면 PAUSED다.

---

# 25. RESUME 원칙

TCP 연결 하나가 복구됐다는 이유만으로 전체 시스템을 RESUME하지 않는다.

예:

```text
Jetson ↔ Pi = UP
Pi ↔ WSL = UP
WSL DB = FAILED
```

이면 여전히 PAUSED다.

최종:

```text
Jetson Link 정상
+
WSL Link 정상
+
WSL 내부 PAUSE 해제
+
Pi 자체 정상
```

일 때만 RUNNING.

---

# 26. Jetson 재연결 시 상태 동기화

Jetson이 Pi에 새 TCP Session으로 재접속했다고 해서 자동 RUNNING하면 안 된다.

예:

```text
WSL이 아직 DOWN

그 상태에서 Jetson reconnect
```

Pi:

```text
새 Jetson session 연결

→ 현재 downstream 상태 확인

WSL Link DOWN
또는
WSL internal PAUSED

→ Jetson에 control(PAUSE)

전체 정상

→ Jetson에 control(RESUME)
```

즉 새 Jetson Session마다 현재 downstream 상태를 동기화한다.

---

# 27. WSL 재연결 시 상태 동기화

Pi↔WSL TCP가 새로 연결됐다고 해서 WSL 내부 정상 상태를 추측하지 않는다.

새 WSL Session 연결 후 WSL 현재 상태를 확인한다.

WSL이:

```text
READY
```

상태를 전달하면:

```text
wsl_internal_paused = false
```

WSL이:

```text
PAUSED
```

상태를 전달하면:

```text
wsl_internal_paused = true
```

그 후 전체 RUNNING 조건을 평가한다.

---

# 28. Reconnect

네트워크 오류는 프로세스 종료 사유가 아니다.

Jetson Link:

```text
connection lost
→ jetson_link = DOWN
→ PAUSE
→ Queue CLEAR
→ 재연결 대기
```

WSL Link:

```text
connection lost
→ wsl_link = DOWN
→ PAUSE
→ Queue CLEAR
→ reconnect 반복
```

Reconnect는 Data Path가 PAUSED인 동안에도 계속 동작한다.

---

# 29. Half-open 연결

다음은 즉시 장애 처리 가능:

```text
recv == 0
send error
recv error
connect error
```

하지만 상대가 죽거나 네트워크가 끊겨도 TCP가 즉시 종료되지 않는 half-open 가능성도 고려한다.

필요 시:

```text
TCP keepalive
또는
lightweight health/control message
```

를 사용한다.

단:

```text
Vision마다 ACK
```

형태로 되돌아가지 않는다.

---

# 30. Thread 구조

역할 기준으로 다음 구조를 사용한다.

```text
① Jetson RX Thread

② WSL TX Thread

③ WSL RX / Control Thread

④ Connection / State Management
```

실제 구현에서는 일부 상태 관리 기능을 Worker에 합칠 수 있다.

단, Data Path와 Control/Recovery Path는 논리적으로 분리한다.

---

# 31. Jetson RX Thread

Jetson socket의 유일한 Receive 담당이다.

```text
Jetson socket

→ 4-byte prefix
→ payload
→ JSON parse
→ type 판별
```

### type = vision

```text
Pi RUNNING
→ Forward Queue push

Pi PAUSED
→ 폐기
```

### type = control

필요한 상태를 처리하고, 다른 노드에 전달해야 하는 정보라면 WSL로 전달한다.

Jetson socket을 여러 Thread가 동시에 `recv()`하지 않는다.

---

# 32. WSL TX Thread

책임:

```text
Forward Queue pop
→ WSL send
```

정상 Vision:

```text
send 완료
→ 바로 다음 Queue pop
```

ACK 대기 없음.

PAUSED:

```text
condition_variable wait
```

WSL send 실패:

```text
wsl_link = DOWN

→ Pi PAUSE
→ Queue CLEAR
→ Jetson에 PAUSE
→ reconnect
```

---

# 33. WSL RX / Control Thread

WSL socket의 유일한 Receive 담당이다.

주요 수신:

```text
control(PAUSE)

control(RESUME)

필요한 health/state
```

예:

```text
WSL
→ PAUSE
reason = database_write_failed
```

Pi:

```text
wsl_internal_paused = true

→ Pi PAUSE
→ Queue CLEAR
→ Jetson에 Control 전달
```

RESUME:

```text
wsl_internal_paused = false

→ 전체 RUNNING 조건 재평가
```

Data Path가 PAUSED여도 이 Thread는 계속 살아 있어야 한다.

---

# 34. Connection / State 관리

최소 관리 대상:

```text
jetson_link

wsl_link

wsl_internal_paused

pi_state

Jetson reconnect 상태

WSL reconnect 상태
```

공유 상태는 Thread-safe하게 관리한다.

사용 가능한 방식:

```text
mutex
atomic
condition variable
```

등 현재 구현 언어와 구조에 맞게 적용한다.

---

# 35. Socket Receive 원칙

하나의 TCP Socket에는 Receive 담당 Thread를 하나만 둔다.

```text
Jetson Socket
→ Jetson RX Thread

WSL Socket
→ WSL RX / Control Thread
```

Control과 Vision 때문에 여러 RX Thread가 같은 Socket에서 경쟁적으로 `recv()`해서는 안 된다.

---

# 36. Socket Send 동기화

같은 socket으로 둘 이상의 실행 경로가 `send()`할 수 있다면 Write를 직렬화한다.

특히:

```text
4-byte prefix
+
JSON payload
```

한 메시지의 prefix와 payload 사이에 다른 메시지가 끼어들어서는 안 된다.

필요 시:

```text
socket별 send mutex
```

또는:

```text
single TX owner
```

구조를 사용한다.

---

# 37. Data / Control 분리

논리적으로:

```text
Data Path
→ Vision

Control Path
→ PAUSE / RESUME / 상태 정보
```

를 분리한다.

Data Path가 PAUSE돼도:

```text
Control RX/TX
Reconnect
Connection Monitoring
State Management
```

은 계속 동작해야 한다.

---

# 38. 장애 발생 기본 처리

Pi가 직접 Link 장애를 관찰했을 때:

```text
1. 해당 Link = DOWN

2. Pi Data Path PAUSE

3. Forward Queue CLEAR

4. 신규 Vision enqueue 금지

5. 살아 있는 반대쪽 Link로
   관측 사실을 Control 전달

6. Control / Reconnect Path 계속 실행
```

---

# 39. 복구 기본 처리

```text
1. 해당 Link 복구 확인

2. connection_restored 기록/Control 생성

3. 상대 상태 확인

4. 전체 RUNNING 조건 평가

5. 모든 조건 정상

6. 필요한 반대편 노드에 RESUME 전달

7. Data Worker wake

8. 새로운 실시간 Vision부터 처리
```

과거 Vision은 복원하지 않는다.

---

# 40. Control message_id

Vision:

```text
Jetson이 message_id 생성
→ Pi는 그대로 유지
→ WSL로 Forward
```

Pi가 생성하는 Control:

```text
Pi가 자체 message_id 생성
```

예:

```text
gateway-pi-01-<boot_id>-<sequence>
```

Control을 다른 노드에서 전달받아 그대로 중계하는 경우 원본 `message_id`를 유지하는 것을 기본으로 한다.

---

# 41. Timestamp 정책

Vision:

```text
Jetson이 생성한 timestamp_ms 그대로 유지
```

Pi가 수신 시각으로 덮어쓰지 않는다.

Pi가 직접 생성하는 Control:

```text
Pi가 해당 상태를 관찰한 Unix timestamp(ms)
```

를 사용한다.

예:

```text
jetson_connection_lost
→ Pi가 disconnect를 관측한 시각

wsl_connection_restored
→ Pi가 연결 복구를 확인한 시각
```

---

# 42. Metrics

Pi에서 최소 확인 가능해야 하는 값:

```text
Jetson received vision msg/s

WSL forwarded vision msg/s

Forward Queue depth

Queue overflow dropped

Discarded on PAUSE

Jetson Link:
UP / DOWN

WSL Link:
UP / DOWN

WSL Internal State:
READY / PAUSED

Pi State:
RUNNING / PAUSED

Current Pause Reason

Jetson reconnect count

WSL reconnect count
```

핵심 성능 기준:

```text
Jetson received msg/s
≈
WSL forwarded msg/s
```

정상 상태에서 Queue가 지속 증가하면 안 된다.

---

# 43. 기존 병목 제거 목표

기존 구조:

```text
send
→ ACK wait
→ next send
```

때문에 throughput이 ACK 왕복시간에 묶였다.

Pi에서 동일 구조를 다시 만들지 않는다.

최종:

```text
Jetson → Pi
continuous vision
no per-message ACK

Pi → WSL
continuous vision
no per-message ACK
```

이다.

---

# 44. Graceful Shutdown

Pi 정상 종료 시:

```text
running flag 종료

condition variable wake

socket shutdown

socket close

worker 종료

thread join

queue clear

resource 정리
```

순서로 안전하게 종료한다.

실시간 시스템이므로 종료 시 Vision Queue 전체를 반드시 drain해서 전송해야 한다는 요구는 없다.

남은 Vision은 폐기 가능하다.

---

# 45. 성공 기준

다음을 모두 검증한다.

```text
1. Jetson → Pi TCP 연결 정상

2. Pi → WSL TCP 연결 정상

3. 4-byte big-endian length-prefix 정상 처리

4. JSON 정상 Parse

5. Jetson Vision 연속 수신

6. Pi → WSL Vision 연속 Forward

7. Vision per-message ACK 없음

8. Vision message_id 유지

9. Vision timestamp_ms 유지

10. Jetson connection lost를 Pi가 직접 감지

11. jetson_connection_lost 사용

12. 원인 미확정 상태에서
    Jetson device down/crash 등으로 단정하지 않음

13. Jetson connection restored 감지

14. jetson_connection_restored 사용

15. Jetson Link 장애 시 Pi PAUSE

16. Jetson Link 장애 시 Queue CLEAR

17. Jetson Link 장애를 WSL에 Control 전달

18. WSL connection lost를 Pi가 직접 감지

19. wsl_connection_lost 사용

20. WSL device down/network failure 등으로
    근거 없이 단정하지 않음

21. WSL connection restored 감지

22. wsl_connection_restored 사용

23. WSL Link 장애 시 Pi PAUSE

24. WSL Link 장애 시 Queue CLEAR

25. WSL Link 장애를 Jetson에 Control 전달

26. WSL 내부 PAUSE Control 수신

27. WSL 내부 장애를 Pi가 임의 판단하지 않음

28. database_write_failed 등
    WSL-originated reason 유지

29. WSL 내부 PAUSE를 Jetson에 전달

30. WSL 내부 RESUME 수신

31. WSL 내부 RESUME을 Jetson에 전달

32. PAUSE 동안 신규 Vision enqueue X

33. PAUSE 동안 Vision backlog X

34. Reconnect는 PAUSE 중에도 계속 동작

35. Jetson 재연결 시 downstream 상태 동기화

36. WSL 재연결 시 WSL 내부 상태 동기화

37. 모든 RUNNING 조건 만족 후에만 RESUME

38. RESUME 후 과거 Vision이 아니라
    현재 시점의 새로운 Vision부터 Forward

39. Pi 자체 DOWN 시
    Jetson/WSL이 각각 connection lost를
    직접 감지할 수 있는 구조

40. 하나의 Socket을 여러 RX Thread가
    동시에 읽지 않음

41. 같은 Socket의 동시 Send로
    framing이 섞이지 않음

42. Data Path PAUSE 중에도
    Control / Reconnect Path는 계속 동작

43. received msg/s 확인 가능

44. forwarded msg/s 확인 가능

45. 정상 상태에서
    received msg/s ≈ forwarded msg/s

46. Queue가 정상 상태에서 지속 포화되지 않음

47. per-message ACK 병목 재도입 없음

48. crash 없음

49. deadlock 없음

50. socket race 없음

51. 정상 shutdown 가능

52. reconnect 후 정상 복구 가능
```

---

# 46. 범위 제외

Pi Gateway에는 다음을 구현하지 않는다.

```text
YOLO inference

Camera

TensorRT

Tracking

track_id

Line Crossing

차량 Count

Vision 통계 분석

최종 SQLite Detection 저장

영상 저장

Snapshot

장애 중 Vision Persistent Storage

Vision backlog 재전송

Vision별 ACK

Stop-and-Wait
```

---

# 47. 최종 Runtime

```text
                         ┌──────────────────┐
                         │      Jetson      │
                         └────────┬─────────┘
                                  │
                       vision ↓   │   ↑ control
                                  │
                    ┌─────────────┴─────────────┐
                    │      Raspberry Pi         │
                    │                           │
                    │  Jetson RX Thread         │
                    │          │                │
                    │          ▼                │
                    │    Forward Queue          │
                    │          │                │
                    │          ▼                │
                    │     WSL TX Thread ─────────────→ WSL
                    │                           │
                    │ WSL RX / Control Thread ◀─────── WSL
                    │          │                │
                    │          └─ control ─────→ Jetson
                    │                           │
                    │ Connection / State        │
                    │ Management                │
                    └───────────────────────────┘
```

---

# 48. 최종 장애 흐름

## Jetson ↔ Pi connection lost

```text
Jetson
→ 직접 감지
→ 자체 PAUSE

Pi
→ 직접 감지
→ 자체 PAUSE
→ Queue CLEAR
→ WSL에 control(PAUSE)
   reason = jetson_connection_lost
```

---

## Pi ↔ WSL connection lost

```text
Pi
→ 직접 감지
→ 자체 PAUSE
→ Queue CLEAR
→ Jetson에 control(PAUSE)
   reason = wsl_connection_lost

WSL
→ 직접 감지
→ 자체 PAUSE
```

---

## WSL 내부 장애

```text
WSL
→ 직접 판단
→ 자체 PAUSE
→ control(PAUSE) → Pi

Pi
→ 자체 PAUSE
→ Queue CLEAR
→ Jetson에 Control 전달
```

---

## Pi 자체 DOWN

```text
Jetson
→ Pi connection lost 직접 감지
→ 자체 PAUSE

WSL
→ Pi connection lost 직접 감지
→ 자체 PAUSE
```

---

# 49. 최종 데이터 정책

```text
NORMAL

Jetson → Pi → WSL
continuous vision
no per-message ACK
```

```text
PAUSE

Forward Queue CLEAR
신규 Vision enqueue X
Vision backlog X

Control / Reconnect는 계속 동작
```

```text
RESUME

모든 관련 상태 정상 확인
→ Data Path wake
→ 현재 시점의 새로운 Vision부터 Forward
```

---

# 50. 최상위 원칙

> **각 노드는 자신이 직접 관찰 가능한 장애를 스스로 판단한다.**

> **TCP 연결 단절만으로 장비 종료, 프로세스 crash, 네트워크 장애 등 실제 원인을 추측하지 않는다. 관찰된 사실은 `*_connection_lost/restored`로 기록한다.**

> **WSL 내부 DB/처리 장애는 WSL만 판단하며, WSL이 PAUSE/RESUME Control을 Pi로 전달한다.**

> **Pi는 Jetson↔Pi와 Pi↔WSL 두 Link의 상태를 직접 판단하고, 반대편 노드가 알아야 하는 상태만 살아 있는 Link를 통해 전달한다.**

> **정상 Vision은 Jetson→Pi와 Pi→WSL 모두 continuous send이며 per-message ACK를 사용하지 않는다.**

> **장애 중 Vision은 보존하지 않는다. PAUSE 시 Queue를 비우고, RESUME 후 현재 시점의 새로운 Vision부터 다시 시작한다.**