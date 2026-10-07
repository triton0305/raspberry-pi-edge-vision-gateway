# Raspberry Pi Edge Vision Gateway

Jetson Vision Client와 WSL Validation Server 사이에서 Vision / Control 메시지를 중계하는 C11 기반 TCP Gateway입니다.

```text
Jetson Edge Vision
        ↓ Vision
Raspberry Pi Gateway
        ↓ Vision
WSL Validation Server

Control: WSL → Pi → Jetson
```

정상 상태에서는 Jetson의 `vision` JSON을 변경하지 않고 WSL로 연속 전달합니다. 장애 또는 PAUSE 상태에서는 대기 Vision을 폐기하며, 복구 후 과거 데이터를 재전송하지 않습니다.

## Build / Test

Linux, CMake 3.16 이상, C 컴파일러와 Python 3가 필요합니다.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

## Run

```bash
./run <wsl_ip> <wsl_port> <jetson_listen_port>
```

예시:

```bash
./run 192.168.0.20 9000 8000
```

또는 직접 실행할 수 있습니다.

```bash
./build/pi_gateway 192.168.0.20 9000 8000
```

포트 번호는 아키텍처상 고정값이 아니며 실행 환경에 맞게 지정할 수 있습니다.

## Core Behavior

- TCP `4-byte big-endian length-prefix + JSON`
- Vision continuous forwarding, per-message ACK 없음
- `message_id`, `timestamp_ms`, Detection payload 변경 없이 전달
- Jetson / WSL 양쪽 연결 상태 독립 감지
- WSL 내부 상태 `UNKNOWN / READY / PAUSED` 관리
- PAUSE 시 Forward Queue 및 pending Vision 폐기
- RESUME 이후 새 Vision만 전달
- 새 WSL 세션에서 첫 Control로 내부 상태 동기화
- Control은 WSL → Pi → Jetson 방향으로 전달
- 장애 중에도 reconnect 및 Control 처리는 계속 수행
- bounded Forward Queue 및 overflow metrics 제공

## Source Layout

| Path | Responsibility |
|---|---|
| `src/gateway.c` | Gateway runtime, connection/state/queue 관리 |
| `src/json.c` | JSON 검증 및 Control 처리 |
| `src/json.h` | JSON 인터페이스 |
| `docs/PROTOCOL.md` | 메시지 및 Control 규격 |
| `docs/VERIFICATION.md` | 검증 항목 |
| `docs/LIVE_VALIDATION.md` | 실제 통합 검증 기록 |
| `docs/SPEC.md` | 상세 개발 명세 |

## Related Repositories

- [Jetson Edge Vision](https://github.com/triton0305/jetson-edge-vision)
- [Jetson Edge Vision Validation Server](https://github.com/triton0305/jetson-edge-vision-validation-server)
