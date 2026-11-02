# M2.2 验证记录

日期：2026-11-02（Asia/Shanghai）。环境沿用 WSL2 Ubuntu 24.04、GCC 13.3.0 和 apt 依赖，未安装依赖或修改系统配置。M1/M2.1 记录保留为历史验证，本文件描述当前异步业务版本。

## 真实命令与最终结果

对本轮修改的 C++ 文件执行 clang-format -i 和 --dry-run --Werror，格式检查通过。检查 18 个修改的代码/构建/配置文件，均无 Tab、行长不超过 100 字符；proto 文件使用 4 空格缩进，新增错误值附带语义注释。

使用已有 preset 构建，新增源文件和 protobuf 枚举触发 Ninja 自动重新配置/生成：

```bash
cmake --build --preset debug
cmake --build --preset release
cmake --build --preset asan
ctest --preset debug
ctest --preset release
ctest --preset asan
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan
setarch x86_64 -R ctest --preset tsan
```

所有构建成功。网络测试经执行环境审批允许本地 loopback socket，未跳过真实网络用例。

| 配置 | 单元测试 | 可控业务 C++ socket | Python socket | CTest |
| --- | --- | --- | --- | --- |
| Debug | 38/38 | 5/5 | 19/19 | 3/3 passed |
| Release | 38/38 | 5/5 | 19/19 | 3/3 passed |
| ASan/UBSan | 38/38 | 5/5 | 19/19 | 3/3 passed |
| TSan，setarch 进程级关闭 ASLR | 38/38 | 5/5 | 19/19 | 3/3 passed |

ASan/UBSan 保留 detect_leaks=1 和 halt_on_error=1 等设置；本轮未报告内存、未定义行为或泄漏错误。TSan 成功运行中未报告数据竞争。

## TSan 原始失败和可复现条件

直接运行 ctest --preset tsan 时，unit 和 async_server_integration 都在进入 GoogleTest 前失败：

```text
FATAL: ThreadSanitizer: unexpected memory mapping 0x70c1fd672000-0x70c1fdb00000
```

该次 CTest 为 0/3；Python 的 19 个用例中 6 个通过、13 个失败，多数失败发生在服务子进程启动阶段并报告相同 mapping 错误。不能将这次直接运行记为通过，也不能据此判定业务代码存在或不存在竞态。

随后实际执行 setarch x86_64 -R ctest --preset tsan，三组测试全部通过。setarch 只调整该次测试进程及子进程的地址布局，不修改 sysctl 或系统级配置；在不允许 personality 设置的环境中，该绕过方法也可能不可执行，应记录真实结果。

GCC 编译 Boost.Asio 头文件时还提示 atomic_thread_fence is not supported with -fsanitize=thread（-Wtsan）。未压制此警告；系统 apt 动态库没有重新插桩。因此即使该次测试通过，也不声称已经证明没有所有竞态。

本机最终完整日志位于 build/<preset>/Testing/Temporary/LastTest.log；最后一次 TSan 日志对应 setarch 成功运行。上面的直接运行错误单独记录于此，防止被后一次日志覆盖后误解。

## 确定性验收对应

| 验收项 | 测试证据 |
| --- | --- |
| 排队数量边界/失败保留请求 | TaskQueue.QueueCountBoundaryPreservesRejectedRequest |
| 排队字节精确边界 | TaskQueue.QueueByteLimitIncludesExactBoundary |
| 出队不绕过总字节预算 | OutstandingBytesCannotBeBypassedByPoppingQueue |
| 完成但 I/O 尚未消费仍计费 | OutstandingCountIncludesCompletedButUnconsumedResult；Dispatcher.PendingIoCompletionKeepsOutstandingCapacityReserved |
| 凭证跨队列生命周期、移动和幂等归还 | TicketCanOutliveQueueAndMoveWithoutDoubleRelease；stop/结果释放用例 |
| 满队列不等待慢业务 | FullQueueDoesNotWaitForBlockedWorker：gate 保持 worker 阻塞时 try_submit 已返回 Full |
| 锁外调用业务/回调 | 可控 worker 及完成回调内 snapshot；无共享锁跨业务或网络发送 |
| 业务在独立线程 | HandlerExceptionProducesCorrelatedErrorAndWorkerSurvives 比较执行线程与提交线程 |
| 业务/完成回调异常 | 失败 ACK 匹配 request_id；下一请求仍完成；完成异常被计数且 worker 继续 |
| 停止丢弃等待任务 | StopDiscardsWaitingTasksButKeepsRunningBudget；StopCancelsQueuedTaskWithoutEarlyRunningRelease |
| 业务完成前无 ACK、尾部顺序 | AsyncServer.NoAckBeforeBusinessCompletesAndBufferedRequestsKeepOrder |
| 慢业务不阻塞其他连接/I/O | AsyncServer.BlockedBusinessDoesNotBlockOtherConnectionsOrIo：同一 I/O 线程、两个 worker |
| 预算为 1 时先释放再恢复 | OutstandingLimitOneAllowsNextBufferedRequestAfterCompletion |
| 真实异常 ACK 与连接继续可用 | BusinessExceptionReturnsErrorAndKeepsConnectionUsable |
| 连接关闭后迟到结果与 work guard | CloseBeforeCompletionDropsLateResultAndReturnsBudget：关闭时预算仍为 1，放行业务后归零 |
| 真实容量拒绝且未执行业务 | Python test_business_byte_capacity_rejects_without_processing，容量为 1 字节，processed=0 |
| 多 I/O/业务线程和回归 | Python test_async_pipeline_with_multiple_io_and_business_threads；原 M1/M2.1 socket 验收继续通过 |
| 退出资源归还 | 每个 Python RunningServer 都检查 business_outstanding 和 outstanding_bytes 最终为 0 |

业务 gate 用 condition_variable 和显式放行控制，不靠生产 sleep 或随机网络分包制造时序。C++ socket 客户端使用带期限的读写以防测试失败挂起；不将测试执行时间当作性能数据。

## 范围和剩余工作

M2.2 的任务队列容量不足仍关闭连接；没有资源释放唤醒、高低水位和统一收发全局预算，留到 M2.3。ACK 仍可能因发送容量不足或断连而丢失，没有持久化、重试或去重。

停止仍丢弃排队任务和未发完 ACK，执行中 Handler 必须有限时返回；协作取消及优雅 drain 截止时间留到 M2.5。没有新增压测或性能报告。

当前 `.git` 为只读空挂载；最终差异通过本轮修改前快照和 git diff --no-index 检查，使用 GitHub 接口提交，不修改挂载或已有项目。
