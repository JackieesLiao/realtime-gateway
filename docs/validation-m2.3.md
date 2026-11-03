# M2.3 验证记录

本轮在 WSL2 Ubuntu 24.04、Linux GCC 13.3 / apt protobuf 3.21.12 工具链执行；没有安装依赖或修改系统配置。沿用 README 中的依赖版本和独立构建目录。本记录只代表 M2.3 的功能验证，不是吞吐/延迟实验。

## 命令与结果

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
cmake --preset tsan
cmake --build --preset tsan
setarch x86_64 -R ctest --preset tsan
```

| 配置 | 单元测试 | C++ 真实 socket | Python 真实 socket | CTest 组结果 |
| --- | --- | --- | --- | --- |
| Debug | 60 通过 | 12 通过 | 21 通过 | 3/3 通过 |
| Release | 60 通过 | 12 通过 | 21 通过 | 3/3 通过 |
| ASan + UBSan + leak 检查 | 60 通过 | 12 通过 | 21 通过 | 3/3 通过 |
| TSan（setarch） | 60 通过 | 12 通过 | 21 通过 | 3/3 通过 |

asan preset 的 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1、UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1；tsan preset 的 TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1。上述 sanitizer 运行未报告错误。apt 动态库未重新使用 sanitizer 编译。

TSan 使用前序阶段已验证的 `setarch x86_64 -R` 方式，仅对测试进程关闭 ASLR；普通运行曾出现 unexpected memory mapping，原始记录见 validation-m2.2.md。本轮不声称普通启动已修复。GCC 编译 Boost.Asio 时仍提示 atomic_thread_fence 不受 TSan 完整支持，因此通过测试不等于证明所有系统库同步均已覆盖。

受限沙箱不允许 loopback 绑定，socket 测试在获得本地 socket 执行权限后运行；测试不因权限问题跳过。运行日志位于各 build/<preset>/Testing/Temporary/LastTest.log（不提交生成文件）。

## 新增验证及证据

| 要求 | 确定性验证 |
| --- | --- |
| 分配前预留 | Frame 准入拒绝只消费 4 字节头，body 容量为零；空输入重试后才接受消息体 |
| 原子多资源申请 | ResourceBudget::HeaderAdmissionReservesEveryStageAtomically；永久失败后动态用量和等待登记均为零 |
| 数量/字节双水位 | 数量仍高时仅缩字节不能恢复；字节仍高时仅降数量不能恢复；queued 同时覆盖两种阈值 |
| 全局硬容量 | CombinedByteLimitCannotBeBypassedByLargeStageLimits、OversizedBodyArithmeticIsRejectedBeforeReservation，以及 socket 小总消息预算 |
| 临时队列满恢复 | worker gate 固定一个执行、一个排队和一个等待；释放后全部收到匹配 ACK |
| 等待连接推进 | 单 pipeline 全局容量下，已等待连接优先于上一所有者的新请求；确定性观察业务顺序 |
| 半帧不被自己卡住 | Receive 容量一帧时先接入两连接并启动读，首连接半帧占高水位仍能收齐，第二连接随后推进 |
| 先消费已有输入 | 单 ACK 预留槽下连续请求逐个恢复，不覆盖尾部，保持 request_id 顺序 |
| 异步写缓冲生命周期 | 使用实际 SendQueue / OutgoingFrame 模拟写回调持有 shared_ptr；clear 不提前归还，最后别名释放后才归零 |
| 关闭时保留在途读信用 | close 移除登记但 Read 凭证仍有效；旧读块未释放不能复用连接槽位 |
| 无漏唤醒/无主动自旋 | 队列释放发生在失败与登记之间仍恢复；无新事件不重复唤醒；关闭后的 pending token 仍计数归还 |
| 请求/结果边界 | 负 enum 的非规范编码规范化可增长但不超过余量；过大 Handler ACK 在 post 前变成有界 INTERNAL_ERROR |
| 关闭和停止 | 部分帧、等待者、执行/排队/待准入任务共存后停止，gate 放行，全部信用和等待数最终为零 |

C++ socket 夹具使用 gate、条件变量和 I/O barrier 控制业务及观察顺序，不用生产 sleep 配置。Python 新增单 ACK 槽 30 条缓冲请求与六连接共享小全局预算；退出必须检查业务在途、budget_messages、budget_bytes、budget_waiters、budget_pending_wakeups 全零。原 M1/M2.1/M2.2 的非法协议、断连、request_id、分片/多帧、容量和信号退出回归继续运行。

慢写的“队列清空但写回调仍持有对象”由确定性所有权测试验证；socket 测试验证真实异步发送与恢复，不声称强制填满内核发送缓冲，也不根据 send/recv 次数推断实际分包。解析分片由单元测试逐切点确定性覆盖。

## 风格、差异与限制

按 AGENTS.md 检查本轮修改；对所有修改的手写 C++ 文件运行项目 clang-format 及 dry-run --Werror，未改无关文件。以本轮开始时文件快照做 git diff --no-index 检查范围，新增文件单独审阅；本地 .git 不可用，不在其中写入。GitHub 提交使用远端当前 main 为父提交并校验分支版本，不强制覆盖历史。

资源限制衡量逻辑数据及对象数量，不是精确 RSS。没有资源等待、半包、空闲或写入期限，慢对端可长期持有信用；已执行 Handler 必须有限时返回。信号退出仍立即取消，未实现 ACK drain。M2.4/M2.5、WS/gRPC、压测、Docker 与 CI 均未纳入本轮验证。
