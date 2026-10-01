# Raspberry Pi Gateway

Jetson Vision Client ↔ Raspberry Pi Gateway ↔ WSL Final Server 사이의 실시간 TCP 중계기입니다. C11, POSIX socket, pthread로 구현했으며 외부 JSON 라이브러리나 다운로드 의존성이 없습니다. 요구사항 원문은 [docs/SPEC.md](docs/SPEC.md)에 있습니다.

정상 Vision은 원본 JSON 바이트 그대로 연속 전달합니다. `message_id`, `timestamp_ms`, detection 필드와 공백도 변경하지 않고, Vision별 application ACK를 생성하거나 기다리지 않습니다. 한 구간 장애 또는 WSL 내부 PAUSE가 발생하면 Forward Queue를 비우고 새 Vision을 폐기합니다. 재연결과 Control RX/TX는 PAUSED 중에도 계속 실행합니다.

## 빌드와 테스트

Linux의 C 컴파일러, CMake 3.16 이상이 필요합니다. 테스트에는 Python 3를 사용합니다.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

테스트 없이 실행 파일만 빌드하려면 `-DBUILD_TESTING=OFF`를 지정합니다.

메모리·정의되지 않은 동작 검증:

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DGATEWAY_SANITIZERS=ON
cmake --build build-sanitize -j2
ctest --test-dir build-sanitize --output-on-failure
```

AddressSanitizer가 실행 시작 전에 주소 공간 할당 오류를 내는 환경에서는 UndefinedBehaviorSanitizer만 선택할 수 있습니다.

```sh
cmake -S . -B build-ubsan -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_SANITIZERS=ON -DGATEWAY_SANITIZER_SET=undefined
cmake --build build-ubsan -j2
ctest --test-dir build-ubsan --output-on-failure
```

## 실행

`gateway` 폴더에서 실행합니다. 아래 WSL 주소를 실제 서버 IP로 바꿉니다.

```sh
./run 192.168.0.20 9000 8000
```

인수 순서는 **WSL 서버 IP → WSL 서버 포트 → Jetson이 접속할 Pi 수신 포트**입니다. 위 명령은 Pi에서 WSL의 9000 포트에 접속하고, Jetson 연결은 Pi의 8000 포트에서 받습니다. Pi는 기본 `0.0.0.0`으로 수신합니다.

`run`은 빌드된 `build/pi_gateway`를 실행하는 스크립트입니다. 직접 실행해도 같습니다.

```sh
./build/pi_gateway 192.168.0.20 9000 8000
```

세 번째 인수를 생략하면 Pi 수신 포트는 기본 9000입니다. 추가 설정이 필요하면 뒤에 옵션을 붙일 수 있습니다.

기존 `--wsl-host IP --wsl-port PORT` 옵션 형식도 사용할 수 있습니다.

`Ctrl+C` 또는 `SIGTERM`으로 종료합니다. 대기 중인 worker를 깨우고 socket shutdown, thread join, socket close와 queue 정리를 수행합니다. 종료 시 남은 Vision은 전송하지 않습니다.

| 옵션 | 기본값 | 의미 |
|---|---|---|
| `--listen-host` | `0.0.0.0` | Jetson 연결을 받을 IP |
| `--listen-port` | `9000` | Jetson 연결 수신 포트 |
| `--wsl-host` | `127.0.0.1` | WSL 서버 IP |
| `--wsl-port` | `9000` | WSL 서버 포트 |
| `--device-id` | `gateway-pi-01` | Pi가 생성하는 Control의 장치 ID |
| `--queue-capacity` | `256` | 대기/부분 송신 Vision의 최대 개수 |
| `--reconnect-ms` | `1000` | WSL 접속 재시도 간격 |
| `--send-timeout-ms` | `5000` | 접속 또는 대기 송신에 진전이 없는 경우 연결 재설정까지의 시간 |

주소는 IPv4/IPv6 리터럴을 사용합니다. 호스트 이름의 DNS 조회로 데이터·복구 처리가 멈추지 않도록 DNS 이름은 받지 않습니다. 장치 ID는 1~95바이트의 영문, 숫자, `_`, `-`를 허용합니다. 동시에 연결된 Jetson은 한 개이며, 추가 연결은 종료합니다.

## 상대 노드의 상태 동기화

**WSL은 최초 연결과 모든 재연결에서 해당 세션의 첫 메시지로 현재 내부 상태 Control을 선제 전송합니다.** Pi는 매 새 세션에서 내부 상태를 UNKNOWN으로 초기화하고, 첫 상태 Control을 기다립니다. TCP 연결 성공이나 이전 세션의 READY를 근거로 Data Path를 재개하지 않습니다.

| WSL 현재 내부 상태 | 세션 첫 Control |
|---|---|
| Vision 저장 가능 (READY) | `action=resume`, `reason=server_ready` |
| 내부 장애로 PAUSED | `action=pause`, `reason=현재 실제 내부 장애 reason` |

Pi는 `resume/server_ready`를 받으면 WSL 내부 READY, `pause/<실제 장애 reason>`을 받으면 내부 PAUSED로 설정한 뒤 전체 RUNNING 조건을 평가합니다. `state_request`는 전송하지 않으며, 별도 요청·응답 handshake 없이 선제 상태 Control만으로 동기화합니다. 중복 상태 Control이 와도 READY/PAUSED 상태 전이를 반복하지 않습니다.

**`pi_connection_restored`는 TCP 연결 복구 관측 event이고 WSL 내부 READY가 아닙니다.** Pi는 해당 reason의 Control을 event로 기록하며, 그 action이 `resume` 또는 `pause`여도 WSL 내부 상태를 바꾸거나 Jetson의 Data Path 명령으로 중계하지 않습니다. 초기 READY는 반드시 `resume/server_ready`로 확인합니다.

이후 WSL 내부 장애/복구의 `pause`/`resume`은 원본 envelope로 Jetson에 전달합니다. `database_recovered`, `server_processing_recovered`, `queue_recovered` 등의 내부 복구 reason도 유지합니다. RESUME은 Jetson 연결, WSL 연결, WSL 내부 READY, Pi Data Path가 모두 정상이고 기존 수신 backlog를 비운 뒤 전달합니다. 구체적인 메시지 예시는 [docs/PROTOCOL.md](docs/PROTOCOL.md)에 있습니다.

Jetson은 매 연결마다 Pi의 PAUSE/RESUME을 따라야 합니다. PAUSE 중 Vision 송신과 자체 대기 Vision을 폐기하고, RESUME 이후 새로 생성한 Vision을 보내야 합니다. Pi 역시 PAUSED 중 수신을 계속해서 Vision을 폐기하며, PAUSED 중 시작된 부분 수신 프레임은 RESUME 뒤 완료되더라도 폐기합니다.

## 실행 구조와 큐 정책

- Jetson RX Thread: Jetson socket의 유일한 `recv()` 담당.
- WSL RX / Control Thread: WSL socket의 유일한 `recv()` 담당.
- WSL TX Thread: WSL의 Vision/Control과 Jetson의 Control을 직렬화해 송신.
- Main: `poll()` 기반 연결 수락, WSL 재접속, 상태 평가, metrics, shutdown 관리.

공유 상태와 큐는 하나의 mutex로 보호하며, worker는 condition variable로 작업을 기다립니다. 소켓은 nonblocking이고 각 작업은 최대 64개 프레임 또는 1 MiB를 처리합니다. 메인은 RX 작업 완료 후 상태를 평가하고 TX를 실행하므로 상태 변경과 Vision 송신 사이에 경쟁이 없습니다. Data Path가 PAUSED여도 Control 송신 작업은 실행합니다.

Forward Queue는 설정한 메시지 수와 16 MiB 중 먼저 도달하는 한도로 제한합니다. 가득 차면 들어오는 새 Vision을 폐기하고 `overflow_dropped`를 증가시킵니다. Control은 Vision보다 먼저 보내되 이미 시작한 프레임 중간에 끼어들지 않습니다. Control 대기 한도는 socket별 128개이며, 이를 초과하면 해당 연결을 재설정해 재동기화합니다.

PAUSE 시 부분 송신 Vision이 있으면 해당 WSL 세션을 닫습니다. TCP 프레임 중간을 삭제한 채 연결을 유지할 수 없기 때문입니다. 새 WSL 세션에서는 상태 동기화부터 다시 수행하고 과거 Vision을 재전송하지 않습니다. 이미 `send()`로 커널에 넘긴 바이트나 상대 노드가 받은 Vision은 되돌릴 수 없습니다. 상대 노드도 PAUSE 시 자신의 대기 데이터와 수신 세션 상태를 정리해야 합니다.

수신 프레임 크기는 1~1,048,576바이트, JSON 깊이는 최대 32, token 수는 최대 4096입니다. Control reason은 디코딩 후 1~255바이트입니다. 잘못된 framing/JSON/envelope는 해당 연결을 닫고 복구 경로로 처리합니다. Vision `data`는 의미를 변경하지 않는 중계 대상으로 다루며, detection의 업무 규칙 검증은 담당하지 않습니다.

## Metrics와 장애 감지

1초마다 stderr에 수신/송신 msg/s, 누적 수신/송신, Queue depth/bytes, overflow dropped, PAUSE 폐기 수, 두 Link의 UP/DOWN, WSL 내부 UNKNOWN/READY/PAUSED, Pi RUNNING/PAUSED, 현재 PAUSE reason, 두 연결의 재접속 횟수를 출력합니다. 최초 연결은 reconnect 횟수에 포함하지 않습니다. `forwarded`는 전체 프레임을 로컬 TCP 송신 버퍼에 전달한 수이며 WSL의 DB 저장 완료를 의미하지 않습니다.

`recv=0`, socket 오류, 접속 실패, 송신 timeout을 관측해 연결 상태를 판단합니다. Linux TCP keepalive는 idle 10초, interval 3초, probes 3회로 설정합니다. 상대 전원 종료·프로세스 crash·네트워크 장애의 실제 원인을 추측하지 않고 `*_connection_lost/restored`를 사용합니다. 장애 감지 시간은 커널과 네트워크 상황에 따라 달라집니다.

테스트별 검증 범위와 실제 상대 노드에서 확인할 항목은 [docs/VERIFICATION.md](docs/VERIFICATION.md)에 있습니다.
