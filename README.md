# realtime-gateway

C++20 实时遥测网关的独立学习重建版本，用于理解实现、面试演示和后续可复现性能实验。已完成 **M1、M2.1 和 M2.2**：设备模拟器 → TCP → 长度帧 → protobuf → 校验/计数 → 携带 request_id 的 ACK。

本仓库的实现与验证记录从本次重建开始，不包含其他电脑上的公司项目代码或历史性能结论。WebSocket、gRPC、收发全局预算、高低水位和性能实验尚未实现，验收计划见 [ROADMAP](ROADMAP.md)。

## Linux 依赖

在 WSL2 Ubuntu 24.04 的 Linux 文件系统和 shell 内操作，不使用 Windows 的编译器、protoc 或库。检查 `which g++ cmake ninja protoc`，应指向 `/usr/bin`。不要在 ROS workspace 或其他已有项目中初始化本项目。

依赖不存在时可自行在 Ubuntu 安装（本次环境已安装，没有执行安装或修改系统配置）：

```bash
sudo apt update
sudo apt install g++ cmake ninja-build libboost-system-dev \
  protobuf-compiler libprotobuf-dev libspdlog-dev nlohmann-json3-dev \
  libgtest-dev python3 python3-protobuf
```

本次实际环境是 WSL2 x86_64、Ubuntu 24.04.4，GCC 13.3.0、CMake 3.28.3、Ninja 1.11.1、Python 3.12.3。apt 包的实装版本：

| 包 | dpkg 版本 |
| --- | --- |
| libboost1.83-dev | 1.83.0-2.1ubuntu3.2 |
| libboost-system-dev | 1.83.0.1ubuntu2 |
| protobuf-compiler / libprotobuf-dev / libprotobuf32t64 | 3.21.12-8.2ubuntu0.3 |
| libspdlog-dev | 1:1.12.0+ds-2build1 |
| nlohmann-json3-dev | 3.11.3-1 |
| libgtest-dev | 1.14.0-1 |
| cmake | 3.28.3-1build7 |
| ninja-build | 1.11.1-2 |
| python3 | 3.12.3-0ubuntu2.1 |
| python3-protobuf | 3.21.12-8.2ubuntu0.3 |

`/usr/bin/protoc --version` 是 libprotoc 3.21.12，CMake 检测 libprotobuf 3.21.12，并强制比较版本；C++ 和 Python 代码均由该 protoc 在 build 目录生成，不提交生成文件。Ubuntu Python 的 protobuf 模块报告 4.21.12，这是该 Python API 的版本方案；其 apt 包来自同一 protobuf 3.21.12 源包。CMake 固定 `/usr/bin/python3`，避免误用缺依赖的 venv。

## 构建、运行与测试

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

在一个终端启动服务：

```bash
./build/debug/gateway_server config/gateway.json
```

另一个终端运行设备：

```bash
./build/debug/device_client 127.0.0.1 9000 42
# request_id=42 result=OK error=
```

Ctrl-C 或 `kill -TERM <pid>` 正常退出。服务端 stdout 输出 spdlog JSON 行，例如 `event=listening/business_result/connection_closed/statistics/stopped`，可以用 `jq` 检索 event、connection_id、request_id 和 reason。客户端是一次请求的同步演示程序，没有重试和网络超时；集成测试给客户端进程设置 5 秒期限。

Release 和 ASan/UBSan 使用独立目录：

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

ASan preset 同时启用 AddressSanitizer、UndefinedBehaviorSanitizer 和 leak 检查，使用非 PIE 的测试可执行文件以固定地址布局。系统 apt 动态库未重新用 sanitizer 编译。测试失败会返回非零；不会因 socket 权限不足而静默跳过。受限执行环境若不允许创建 loopback socket，需要允许本地 socket 后重新运行集成测试。最新真实命令及结果见 [M2.2 验证记录](docs/validation-m2.2.md)。

TSan 使用单独目录，不能与 ASan 同时启用：

```bash
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan
```

本次 WSL2/GCC 13 环境直接运行 TSan 遇到 unexpected memory mapping。以下命令只对测试进程关闭 ASLR，实际已通过，不修改系统配置：

```bash
setarch x86_64 -R ctest --preset tsan
```

CTest 包含普通 GoogleTest、带可控处理器的 C++ socket 测试和 Python socket 测试。C++ 处理器注入用于确定性控制业务执行，不增加生产延迟配置；所有网络测试只绑定 loopback。sanitizer 的实际可执行性和限制见验证记录。

## VS Code 编辑器与代码风格

在 VS Code 的 WSL 窗口中打开仓库，C/C++ 扩展使用仓库内的 `.vscode/settings.json`：Linux `/usr/bin/g++`、C++20 和 `build/debug/compile_commands.json`。先执行 `cmake --preset debug` 和 `cmake --build --preset debug`，生成编译数据库及 protobuf 头文件。

`std::span` 和 `std::jthread` 需要 C++20。若编辑器按 C++17 解析，`feed` 的声明会先解析失败，随后可能把参数 `bytes`、`consume` 标记为未定义；不要通过声明同名全局变量绕过。项目明确要求 C++20，符合 AGENTS.md 中“项目另有指定时遵循项目”的约定。

若构建通过但仍显示旧红线，运行命令面板里的 `C/C++: Reset IntelliSense Database`，必要时再执行 `Developer: Reload Window`。可用 `C/C++: Log Diagnostics` 检查实际编译器、C++ 标准和编译数据库路径；不要选用 Windows 的 MSVC 配置。当前自动化验证覆盖编译数据库和编译结果，无法直接读取用户窗口的实时诊断。

仓库 `.clang-format` 对应 AGENTS.md：4 空格、100 列、控制流加花括号、展开单行函数和 lambda。使用已安装的 clang-format 18.1.3 格式化手写 C++ 文件，不格式化 build 下的生成文件：

```bash
clang-format -i include/gateway/*.hpp src/*.cpp tests/*.cpp
clang-format --dry-run --Werror include/gateway/*.hpp src/*.cpp tests/*.cpp
```

## 配置与边界

配置文件是 JSON 对象，字段可省略并使用默认值；未知键、负数、小数或越界值启动失败。示例见 [config/gateway.json](config/gateway.json)。

| 字段 | 默认值 | 可配置范围/含义 |
| --- | --- | --- |
| listen_address | 127.0.0.1 | 本机可绑定的 IPv4/IPv6 地址字面值 |
| port | 9000 | 0..65535；0 让系统分配端口，日志报告实际值 |
| io_threads | 2 | 1..64，一个 io_context，包括主线程 |
| max_connections | 1024 | 1..100000；超过立即拒绝新连接 |
| business_threads | 2 | 1..64，额外的业务执行线程 |
| max_business_queue_messages | 128 | 1..65536，仅排队任务 |
| max_business_queue_bytes | 8388608 | 1..268435456，仅排队请求的序列化字节 |
| max_business_outstanding_messages | 256 | 1..65536，排队/执行/等待回送总数 |
| max_business_outstanding_bytes | 16777216 | 1..268435456，总在途请求的序列化字节 |
| max_body_bytes | 1048576 | 1..16777216，不含 4 字节包头 |
| max_receive_body_bytes | 1048576 | 1..16777216，单连接待完成消息体上限 |
| max_send_messages | 64 | 1..65536，含正在写的帧 |
| max_send_bytes | 262144 | 1..67108864，含帧头和正在写的帧 |

业务 Accepted 后，同连接暂停后续投递和新读，结果回到 strand 后恢复已有输入。业务队列/总在途容量不足时关闭连接；发送容量超限仍关闭。两个 body 限制中较小者生效。业务预算以 Request::ByteSizeLong() 计费，不含 TCP 包头，也不等于 RSS；接收、发送和业务统一的全局预算留到 M2.3。

## 关键设计

Transport 独占帧解析和网络对象；业务 Router/Dispatcher 只接受 protobuf 类型。每个 socket 使用自己的 strand，保证一个读链和一个写链；发送 buffer 由写回调独立持有，取消连接后仍活到操作完成。ACK 在业务校验与计数完成后才创建，但不表示持久化，断连时也不保证 ACK 送达。

Dispatcher 使用显式有界队列和固定 worker 执行 Router，完成结果 post 回原连接 strand。总在途预算覆盖排队、执行和等待 I/O 消费的结果，防止把无界积压转移到完成回调。每连接最多一个业务在途，不跨线程访问连接成员；停止后在控制线程 join，不在 I/O 回调里等待 worker。

- [架构、线程、消息流与生命周期](docs/architecture.md)
- [线协议、ACK 和错误策略](docs/protocol.md)
- [M1 验证记录与用例对应](docs/validation-m1.md)
- [M2.1 变更范围与阅读路径](docs/changes-m2.1.md)
- [M2.1 验证记录](docs/validation-m2.1.md)
- [M2.2 变更范围与阅读路径](docs/changes-m2.2.md)
- [M2.2 验证记录](docs/validation-m2.2.md)

当前异步业务路径优先阅读 BoundedTaskQueue::try_push（src/bounded_task_queue.cpp）、Dispatcher::run_worker（src/dispatcher.cpp）和 Session::handle_business_result（src/server.cpp）。分别理解容量准入、锁外执行与凭证回送、ACK 排队和恢复顺序；接收基础继续阅读 FrameDecoder 和 BufferedInput。

M2.2 已接入真实异步业务暂停与恢复，但资源不足仍关闭连接。当前退出丢弃排队任务并关闭连接，不承诺排空 ACK；执行中 Handler 必须有限时返回。高低水位、统一全局预算、限流、心跳/半包超时和 graceful drain 仍在后续步骤；没有压测或 Docker/CI，也没有未经测量的性能数字。
