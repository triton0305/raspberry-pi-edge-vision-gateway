# 검증 기록

2026-10-01, Linux aarch64, GCC 14.2.0, C11 환경에서 빌드 및 localhost TCP 상대 노드 모의 테스트를 수행했습니다.

## 자동 테스트

| CTest | 검증 내용 |
|---|---|
| `json_parser` | JSON 구조, 중첩, 숫자, UTF-8, Unicode escape/surrogate, 문자열 quote/decode, 중복 key, NUL·잘못된 UTF-8 거부, token/depth 제한 |
| `integration` | 분할 length-prefix/payload 수신, ACK 없이 연속 전달, Vision 원본 바이트 보존, 초기 WSL 상태 대기, PAUSE 폐기, 두 Link 장애/복구, Jetson 재접속 상태 동기화, WSL 재접속 내부 상태 초기화, 잘못된 JSON/초과 길이 복구, SIGTERM 종료 |
| `session_sync` | 최초 연결·모든 재연결의 선제 `resume/server_ready` 또는 실제 장애 PAUSE, state_request 없이 동기화, 이전 세션 READY 초기화, `pi_connection_restored` event와 내부 READY 구분, 중복 상태의 안전한 재적용, 원본 Control 보존 |
| `recovery` | PAUSED 중 시작된 부분 수신 Vision 폐기, 원본 PAUSE/RESUME envelope 보존, 임의 reason 문자열의 안전한 재동기화, 중복 Jetson 연결 거부, bounded queue overflow/metrics, 부분 송신 중 PAUSE 세션 재설정, WSL 초기 미접속 재시도, 0-byte/중복 key/NUL/UTF-8 오류, 양방향 추가 Control 중계, 5,000개 연속 Vision 전달 및 수신·송신 누적 일치/queue=0, WSL 송신 정체 timeout 복구, Jetson 부재 중 RESUME 보류, RX 작업 한도를 넘는 PAUSED backlog 폐기 |

Release 빌드와 UndefinedBehaviorSanitizer 빌드에서 전체 테스트를 통과했습니다. 컴파일 시 `-Wall -Wextra -Wpedantic -Werror`를 사용합니다. UBSan은 오류를 복구하지 않고 즉시 실패하도록 설정합니다.

AddressSanitizer를 포함한 빌드는 성공했지만, 이 실행 환경에서는 프로그램 시작 전에 ASan 런타임의 주소 영역 할당이 실패했습니다:

```text
sanitizer_allocator_primary64.h:131
kSpaceBeg == address_range.Init(...) CHECK failed
```

이는 애플리케이션 메모리 검사 결과가 아니므로 ASan 통과로 기록하지 않습니다. 다른 ASan 지원 환경에서는 README의 `build-sanitize` 명령으로 실행할 수 있습니다.

## 실제 Jetson / WSL 연결에서 남은 확인

자동 테스트의 상대 노드는 모의 TCP 클라이언트/서버입니다. 실제 Jetson Vision Client와 WSL Final Server의 주소 및 실행 환경은 제공되지 않았으므로 두 실제 프로그램과의 종단 테스트는 수행하지 않았습니다.

1. Jetson이 Pi 수신 IP/port에 연결하고, PAUSE 시 자체 Vision Queue를 폐기하며 새 session의 상태 동기화를 따르는지 확인합니다.
2. WSL이 최초 연결과 모든 재연결에서 첫 메시지로 `resume/server_ready` 또는 `pause/<현재 실제 내부 장애 reason>`을 선제 전송하고, 내부 DB 장애/복구의 reason과 message_id를 생성하는지 확인합니다.
3. 실제 발생 Vision msg/s에서 `received ≈ forwarded`, 지속적인 Queue 증가 및 overflow가 없는지 확인합니다. 자동 테스트의 localhost throughput은 실제 네트워크·DB 성능 보장이 아닙니다.
4. 실제 네트워크 단절과 장비 전원 종료를 각각 발생시켜 TCP keepalive 감지 및 재연결을 확인합니다. 자동 테스트는 실제 socket close와 송신 정체를 검증했으며 물리 네트워크 half-open 상황까지 재현하지 않았습니다.
5. WSL은 이미 TCP로 수신한 데이터를 PAUSE 정책에 맞게 처리해야 합니다. Pi의 이미 완료된 `send()`를 소급해서 취소하는 기능은 없습니다.
