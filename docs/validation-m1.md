# M1 验证记录

日期：2026-11-02（Asia/Shanghai）。这是本次独立实现的运行记录，没有沿用公司项目的历史结果。

## 环境与真实命令

`uname -a`：Linux LAPTOP-33PRCHEL，6.6.87.2-microsoft-standard-WSL2，x86_64。
`/etc/os-release`：Ubuntu 24.04.4 LTS。GCC 13.3.0，CMake 3.28.3，Ninja 1.11.1，protoc/libprotobuf 3.21.12；apt 包明细见 README。

先检查了当前目录和各级父目录的 AGENTS.md，无适用规则。当前目录是独立空目录；旁边的 ROS 教程/工作区未修改。实际执行了：

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

配置和编译均成功，未执行 apt 安装。补充用例后重新构建三个 preset，再验证最终代码。

第一次受限沙箱内执行 Debug ctest 时，unit 通过，socket_integration 因 `open: Operation not permitted` 失败（12 个 socket 用例无法创建 socket，1 个配置用例通过）。随后通过执行环境审批，允许 loopback socket 后重新运行完整测试。没有修改系统配置，也没有跳过 socket 测试。

最终结果：

| preset | GoogleTest | Python 真实 socket 集成 | CTest 总结果 |
| --- | --- | --- | --- |
| debug | 13/13 | 16/16 | 2/2 passed |
| release | 13/13 | 16/16 | 2/2 passed |
| asan | 13/13 | 16/16 | 2/2 passed |

ASan/UBSan preset 使用 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` 和 `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`。本次没有检测到 sanitizer 错误。apt 提供的第三方动态库未重新插桩，未执行 TSan；测试通过不构成不存在所有竞态/内存问题的证明。

完整本机日志在 `build/<preset>/Testing/Temporary/LastTest.log`（build 被忽略，不上传二进制/生成代码/机器路径日志）。这是功能验证，不是压测；测试耗时不作为吞吐或延迟指标。

## 验收用例对应

| 验收项 | 确定性单测/真实 socket 用例 |
| --- | --- |
| 正常 ACK 与 request_id | Business.AckAfterCountersAndCorrelation；test_request_ack_and_client，包含 uint64 最大值与 C++ 客户端 |
| 包头、消息体分段 | Frame.EveryTwoPartSplit 穷举两个片段切点；Frame.ByteByByte；test_fragmented_header_and_body |
| 多帧和不完整尾包 | Frame.MultipleFramesAndIncompleteTail；test_coalesced_and_incomplete_tail |
| 超长声明分配前拒绝 | Frame.RejectsOversizeBeforeAllocatingBody 检查 capacity 不变且消费者未调用；test_oversize_header_only 只发送 0xffffffff 头 |
| 空帧 | Frame.EmptyFrameAndConsumerStop；test_empty_frame |
| 非法 protobuf | Protobuf.RejectsTruncatedLengthDelimitedField；test_invalid_protobuf |
| 未知类型/业务错误 | Business.ValidationAndUnknownTypes；test_unknown_type_and_business_validation，随后合法请求继续成功 |
| 中途断开 | test_peer_disconnects_mid_header_mid_body_and_during_reply；后续健康连接仍能处理 |
| 多连接独立工作 | test_multiple_connections，24 个连接任务、最多 12 个并行，每连接 5 个匹配 ACK |
| 接收容量 | Frame.RejectsReceiveCapacityBeforeAllocatingBody、ExactReceiveAndFrameBoundary；test_receive_capacity_header_only、test_frame_limit_boundary |
| 发送数量/字节容量 | SendQueue 两个单测覆盖在途帧计费、精确边界、拒绝、释放和 buffer 所有权；test_send_capacity_single_ack_too_large 覆盖真实断连 |
| 连接容量 | test_connection_capacity；已接入的连接仍可工作 |
| 无隐式去重 | test_duplicate_ids_are_processed_again；重复 ID 得到两次 ACK，统计 processed=2 |
| 配置错误 | Config.RejectsUnsafeSettingsAndTypos；test_bad_config_fails，非零退出 |
| 正常退出 | 每个 RunningServer 清理都检查退出码和 stopped；test_signal_exit_with_live_partial_reads 分别验证 SIGINT/SIGTERM 取消活动半包读 |

socket 测试发送不同片段，但不声称服务端 recv 实际按这些片段返回；解析分片正确性由直接调用 feed 的单测确定。测试使用 port=0，从 listening JSON 事件获取端口，不通过“先找空闲端口再绑定”制造竞争。

发送队列数量限制在单测中确定性验证，没有使用随机 flood 来猜测内核写缓冲是否填满。在途帧独立所有权也有单测；真实断连和退出用例结合 sanitizer 覆盖生命周期，但没有声称精确控制了所有取消时序。

## 本阶段限制与后续验证

M1 没有业务线程、跨线程 completion、完整背压、全局预算、限流、心跳/半包超时或 ACK drain。这些不是未执行的 M1 测试，而是尚未实现的 M2 范围。WebSocket/gRPC、压力与性能数据、Docker 和 CI 同样留在后续阶段。

本执行环境把空 `.git` 挂载为只读，普通 `git status` 因而提示不是 Git 仓库。未修改挂载或覆盖 `.git`；通过用户指定的 GitHub 仓库接口分批提交源代码与文档。该限制影响本地 Git 元数据，不影响 Linux 构建和真实 socket 验证。后续在普通 WSL 目录 clone 远端即可获得常规 Git 工作区。
