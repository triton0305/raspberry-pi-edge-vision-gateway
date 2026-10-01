# Gateway wire contract

모든 메시지는 **4-byte unsigned big-endian JSON 길이 + UTF-8 JSON payload**입니다. 길이에는 prefix를 포함하지 않습니다. prefix와 payload 모두 partial read/write를 허용하며, 하나의 TCP 연결에서 메시지들이 붙어서 들어와도 처리합니다.

## 공통 envelope

```json
{
  "version": 1,
  "type": "control",
  "device_id": "server-wsl-01",
  "message_id": "server-wsl-01-boot-000001",
  "data": {
    "timestamp_ms": 1790812345800,
    "action": "resume",
    "reason": "server_ready"
  }
}
```

필수 envelope 필드는 `version=1`, `type`, 비어 있지 않은 문자열 `device_id`/`message_id`, object `data`입니다. `type`은 `vision` 또는 `control`입니다. Control의 `action`과 `reason`은 비어 있지 않은 문자열입니다. 문자열 escape와 UTF-8을 처리하며, 모호한 중복 JSON key는 허용하지 않습니다.

## 새 WSL 세션 — 확정 계약

**WSL이 최초 연결과 모든 재연결에서 해당 세션의 첫 메시지로 현재 내부 상태 Control을 선제 전송합니다.** Pi는 WSL TCP connect 성공 후 `wsl_internal=UNKNOWN`으로 대기합니다. 이전 세션의 READY를 재사용하지 않습니다.

WSL 내부 READY일 때 WSL → Pi의 첫 메시지:

```json
{
  "version": 1,
  "type": "control",
  "device_id": "server-wsl-01",
  "message_id": "server-wsl-01-boot-000001",
  "data": {
    "timestamp_ms": 1790812345800,
    "action": "resume",
    "reason": "server_ready"
  }
}
```

WSL 내부 PAUSED일 때 WSL → Pi의 첫 메시지:

```json
{
  "version": 1,
  "type": "control",
  "device_id": "server-wsl-01",
  "message_id": "server-wsl-01-boot-000002",
  "data": {
    "timestamp_ms": 1790812345800,
    "action": "pause",
    "reason": "database_write_failed"
  }
}
```

PAUSED의 reason에는 `database_write_failed`, `server_processing_failed`, `queue_overload` 등 **현재 실제 내부 장애 reason**을 사용합니다.

| 새 세션의 수신 상태 Control | Pi 내부 상태 | 다음 처리 |
|---|---|---|
| `resume / server_ready` | READY | 전체 RUNNING 조건 평가 |
| `pause / <실제 내부 장애 reason>` | PAUSED | Data Path PAUSE 유지, 원본 PAUSE 중계 |
| 상태 확인 전 다른 내부 복구 reason의 `resume` | UNKNOWN 유지 | 올바른 초기 상태 Control 대기 |

Pi는 `state_request`를 보내지 않습니다. WSL은 Pi의 요청이나 다른 Control을 읽기 전에 선제 상태 Control을 보낼 수 있으며, 이 메시지만으로 상태 동기화가 완료됩니다. 이후 내부 장애 복구에는 `resume/database_recovered`, `resume/server_processing_recovered`, `resume/queue_recovered` 등을 사용합니다.

동일 상태 Control을 여러 번 수신해도 READY/PAUSED 전이를 반복하거나 새 세션을 생성하지 않습니다. 원본 PAUSE/RESUME이 반복 중계될 수 있으므로 상대 노드도 동일 상태 명령을 재적용해도 안전하게 처리해야 합니다. `resume/server_ready` 원본도 다른 RESUME과 마찬가지로 모든 RUNNING 조건이 만족될 때 Jetson에 중계하며, message_id/reason/timestamp를 유지합니다.

### 연결 복구 event와 내부 READY

| 의미 | Control reason | 내부 READY 판단 |
|---|---|---|
| WSL이 관측한 Pi↔WSL TCP 연결 복구 event | `pi_connection_restored` | READY/PAUSED를 변경하지 않음 |
| WSL이 Vision을 저장할 수 있는 현재 내부 상태 | `resume / server_ready` | WSL 내부 READY로 설정 |
| Pi가 직접 관측한 WSL TCP 연결 복구 event | `wsl_connection_restored` | 상태 Control 수신 전까지 UNKNOWN 유지 |

`pi_connection_restored`를 담은 Control은 event로 기록합니다. action이 `resume`이어도 내부 READY로 해석하거나 Jetson RESUME 명령으로 중계하지 않으며, action이 `pause`여도 WSL 내부 장애로 해석하지 않습니다. 이 event만 수신한 새 세션은 계속 초기 상태 Control을 기다립니다.

## 장애와 복구

| 관측 또는 수신 | Pi 처리 | 살아 있는 상대 구간으로 전달 |
|---|---|---|
| Jetson 연결 종료 | PAUSE, Vision Queue CLEAR | WSL에 `pause / jetson_connection_lost` |
| Jetson 새 연결 | 현재 WSL 상태 평가, Jetson 상태 동기화 | WSL에 `pause / jetson_connection_restored`, 전체 정상 시 `resume / jetson_connection_restored` |
| WSL 연결 종료 | PAUSE, Vision Queue CLEAR, 재접속 | Jetson에 `pause / wsl_connection_lost` |
| WSL 새 연결 | 내부 상태 UNKNOWN, WSL의 선제 첫 상태 Control 대기 | Jetson에 `pause / wsl_connection_restored`; `resume/server_ready` 수신과 나머지 조건 확인 후 WSL의 원본 RESUME 중계 |
| WSL 내부 `pause` | 내부 PAUSED, Vision Queue CLEAR | Jetson에 원본 PAUSE 중계 |
| WSL 내부 `resume` | 내부 READY, 전체 조건 평가 | 전체 정상일 때 Jetson에 원본 RESUME 중계 |

`pause / *_connection_restored`는 연결 복구 사실을 알리면서 전체 Data Path는 아직 대기한다는 의미입니다. `reason`만 보고 송신을 시작해서는 안 되며 `action`을 따라야 합니다. Jetson이 없는 동안 받은 RESUME은 보류하고, 새 Jetson 세션의 조건이 모두 정상일 때 전달합니다. 이후 WSL PAUSE 또는 WSL 세션 종료가 발생하면 보류한 RESUME은 폐기합니다.

## Vision 전달

Jetson의 `vision` payload는 WSL로 byte-for-byte 전달합니다. Gateway가 detection ID, Vision message_id, timestamp를 생성하거나 덮어쓰지 않습니다. WSL에서 Vision을 Pi로 보내는 것은 계약 위반으로 처리합니다. Vision별 ACK는 없습니다.

Pi가 생성한 Control에는 자체 `device_id`, `<device>-<boot_id>-<sequence>` message_id, 관측 시점의 Unix ms timestamp를 사용합니다. 원본 Control 중계에서는 original message_id, reason, timestamp와 payload를 유지합니다. 알 수 없는 추가 Control action은 반대편으로 중계하고, WSL 내부 READY/PAUSED 상태를 임의로 변경하지 않습니다.
