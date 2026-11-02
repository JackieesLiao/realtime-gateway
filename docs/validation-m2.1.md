# M2.1 验证记录

日期：2026-11-02（Asia/Shanghai）。本轮验证接收解析的单帧输出、保留尾部和暂停/恢复基础；环境及 apt 版本沿用 [M1 验证记录](validation-m1.md) 和 README。

## 真实命令与结果

执行 clang-format 18.1.3 格式化及检查本轮修改的 C++ 文件：

```bash
clang-format -i include/gateway/frame.hpp include/gateway/buffered_input.hpp src/frame.cpp src/buffered_input.cpp src/server.cpp tests/unit.cpp tests/buffered_input_test.cpp
clang-format --dry-run --Werror include/gateway/frame.hpp include/gateway/buffered_input.hpp src/frame.cpp src/buffered_input.cpp src/server.cpp tests/unit.cpp tests/buffered_input_test.cpp
cmake --build --preset debug
cmake --build --preset release
cmake --build --preset asan
ctest --preset debug
ctest --preset release
ctest --preset asan
```

使用已有配置目录构建，Ninja 自动重新运行 CMake 登记新增源文件。最终格式检查通过；检查了 9 个修改的代码/构建文件，无 Tab，行长不超过 100 字符。因执行环境限制 socket 创建，ctest 经执行环境审批在允许本地 loopback socket 的环境运行，没有跳过网络测试。

| preset | GoogleTest | Python 真实 socket | CTest |
| --- | --- | --- | --- |
| debug | 26/26 | 17/17 | 2/2 passed |
| release | 26/26 | 17/17 | 2/2 passed |
| asan | 26/26 | 17/17 | 2/2 passed |

ASan 同时启用 UBSan；测试 preset 保持 detect_leaks=1、halt_on_error=1 等原有设置。本轮未报告 sanitizer 错误。没有新增工作线程或执行性能实验，也未运行 TSan。

## 验收对应

| 行为 | 用例与证据 |
| --- | --- |
| 包头/消息体所有二段切点 | Frame.EveryTwoPartSplit：每次消费量准确，完整帧只取走一次 |
| 逐字节输入 | Frame.ByteByByte；BufferedInput.ByteByByteReadsPreserveProgress |
| 每次最多一帧、多帧加尾包 | Frame.MultipleFramesAndIncompleteTail：依次消费 first/second，再等待 third 剩余数据 |
| 分配前拒绝超长声明 | Frame.RejectsOversizeBeforeAllocatingBody/RejectsReceiveCapacityBeforeAllocatingBody：capacity 不增长，消费量停在包头 |
| 完整帧未取走 | Frame.ReadyFrameRemainsUntilTaken：重复 feed 不消费新输入，不覆盖当前 body |
| 消息体所有权 | take_body 后再解析新帧，之前取得的字符串内容仍保持；无完整帧时禁止取走 |
| 中间帧暂停与恢复 | BufferedInput.PauseAtMiddleFrameThenDrainExistingTail：B 暂停，C/尾包保留；多次重试后各帧只接受一次 |
| 输入末尾恰好是暂停帧 | BufferedInput.PausedFrameAtInputEndStillPreventsRead：即使 unread_bytes=0，也不能覆盖待接管帧 |
| 半包跨读缓冲复用 | BufferedInput.PartialBodySurvivesReadBufferReuseAndPause；LargeBodySpansSeveralFixedReadBuffers |
| 关闭/消费者停止 | StopWhilePausedIsTerminal、ConsumerStopDoesNotProcessFollowingFrames；终止后不再调用消费者 |
| 同步关闭优先级 | ConsumerTriggeredStopTakesPriorityOverItsReturnValue：关闭后不能被 Accepted/Paused 返回值覆盖 |
| 覆盖旧输入与越界提交 | RejectsOverwritingUnreadBytesAndOversizedCommit：明确抛错，禁止重置旧输入 |
| 错误后的尾部 | ErrorDoesNotConsumeFollowingFrameOrCallConsumer：后续合法帧不被继续处理 |
| 真实大消息与后续请求 | test_body_spanning_multiple_read_buffers：12000 字节 payload 后仍正确匹配后续 ACK |
| 原有网络行为 | 原 16 个 socket 用例继续通过，包括分片、多帧尾包、非法 protobuf、容量、断连、多连接及信号退出 |

单测直接操控 BufferedInput 的生产状态机，不借用系统分包结果。socket 用例只验证最终协议行为，没有断言某次 send 对应某次 recv。

## 验证范围与限制

本轮的确定性暂停/恢复验证针对 Session 实际持有的 BufferedInput。Session 内部 pause_reading/resume_reading 已接入 strand 和统一推进入口，但正常同步业务不调用它们；未声称已通过真实异步业务完成回送、在途读期间资源水位切换或全局预算竞争测试。这些验证随 M2.2/M2.3 的实际调度来源补齐。

输入容量仍是固定读块加一个受长度限制的消息体，应用容量不等于精确 RSS。完整背压、业务线程池、限流、超时和优雅 drain 均未实现。

当前 `.git` 仍是只读空挂载，使用修改前快照与 git diff --no-index 检查差异，并通过 GitHub 接口提交；不修改挂载。完整本机测试日志位于 build/<preset>/Testing/Temporary/LastTest.log，不提交 build 产物。
