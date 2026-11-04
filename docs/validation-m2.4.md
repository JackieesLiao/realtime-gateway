# M2.4 验证记录

本轮在 WSL2 Ubuntu 24.04、Linux GCC/apt 工具链执行。沿用 README 的依赖和独立构建目录，没有安装依赖、修改系统配置或引入 Windows 工具。C++/Python protobuf 仍由匹配的 /usr/bin/protoc 生成。

## 实际命令与结果

```bash
cmake --build --preset debug
ctest --preset debug
cmake --build --preset release
ctest --preset release
cmake --build --preset asan
ctest --preset asan
cmake --build --preset tsan
setarch x86_64 -R ctest --preset tsan
python3 -m py_compile tests/integration.py
```

CMake 在新增实现/测试及 protobuf 变化后自动重新配置、生成代码。四种构建均成功；最终结果：

| 配置 | 单元测试 | C++ socket | Python socket | 分析器回归 | CTest |
| --- | --- | --- | --- | --- | --- |
| Debug | 81 通过 | 15 通过 | 29 通过 | 3 通过 | 4/4 组通过 |
| Release | 81 通过 | 15 通过 | 29 通过 | 3 通过 | 4/4 组通过 |
| ASan/UBSan/leak | 81 通过 | 15 通过 | 29 通过 | 3 通过 | 4/4 组通过 |
| TSan（setarch） | 81 通过 | 15 通过 | 29 通过 | 3 通过 | 4/4 组通过 |

ASan/UBSan 由 preset 设置 detect_leaks=1、halt_on_error=1 等参数；TSan 使用既有进程级 ASLR 设置，不修改系统 ASLR。上述运行没有 sanitizer 报错。GCC 编译 Boost.Asio 时仍提示 atomic_thread_fence 的 TSan 支持限制，apt 动态库未重新插桩；测试通过不能证明所有系统库同步均已覆盖。普通 TSan 启动的既有 mapping 问题见前序验证记录，本轮不声称已修复。

socket 测试在允许 loopback 绑定的执行环境运行，没有因为权限跳过。完整输出位于 build/<preset>/Testing/Temporary/LastTest.log，不提交生成文件。

## 可控时间单测

ConnectionPolicy 由调用者传时间点，测试不修改系统时间、不添加生产 sleep，也不依赖真实 timer 精确调度：

- 3 个令牌/秒的分数补充在 333333333ns 仍不足，333333334ns 满足；等待向上取整。
- 消息/字节双桶均满足才扣费；peek 和失败的 consume 不扣掉另一桶；超过 burst 为永久失败。
- 0 速率禁用对应维度，大成本不参与无意义乘法；长时间闲置先判断填满。额外构造 2^55ns × 1e9 在 64 位会回绕为零的边界，验证乘法前保护。
- 首字节启动 frame，重复 frame_started 不续期；暂停前的已用时间保留，恢复只获得剩余额度。
- idle 不被无效帧或服务端暂停续期；完整合法请求刷新后，旧期限到来不关闭当前连接。
- 延迟解析使用原完整帧到达时间，不能把恢复时刻当作新活动。
- Rate/Resource 切换及反复唤醒保留原等待起点；等待超时与令牌补充是独立事件。
- write 不因服务端暂停延后，完成后旧期限无效，下一写链重新计时。
- 0 期限不创建 timer；非法配置启动失败；同时到期按 write/wait/frame/idle 固定顺序分类。

BufferedInput 新测试验证首字节和完整帧事件在分片、预算拒绝、重复 process 下各报告一次。业务测试验证心跳空 payload、公共字段、独立 heartbeats 统计；配置测试覆盖新增字段范围。ResourceBudget 测试验证撤销本地限流等待后，后续全局等待者可继续推进。

## 真实 socket 验证

Python 新增七个用例：心跳及最小客户端；静默连接空闲超时；确定不完整的包头/消息体超时；消息速率等待期限且第二条不执行；令牌补充后缓冲请求按序完成；独立字节速率；超过 burst 的包头立即拒绝。

C++ 新增三条受控路径：

- 全局 Receive 仅一帧，通过预算快照确认首连接已持有半帧；第二连接等待超时，首连接仍能收齐并得到 ACK。
- worker gate 阻塞业务，idle 关闭连接；放行业务后迟到结果丢弃，队列及全局预算归零。
- idle=1000ms，第一次请求后用明确的 I/O timer 里程碑等待 400ms，再发送有效请求；继续等待 700ms，跨过旧期限后仍能完成第三条请求。这个用例验证实际 timer 桥接，精确期限判定另由纯时间测试覆盖。

所有 C++ 夹具与 Python 退出检查继续验证总预算、业务在途、等待者、待处理唤醒最终为零。原分片、尾包、超长头、非法 protobuf、多连接、队列满恢复、断连和停止回归保留。socket 用例不断言 send/recv 对应关系。

## 覆盖范围与限制

write 超时判定由可控时间策略测试精确覆盖，真实 socket 验证同一个 timer 桥接的 idle/rate/resource 与关闭路径；本轮没有强制填满内核发送缓冲，不声称已经完成内核慢读压力实验。前序 OutgoingFrame 所有权测试继续覆盖清空队列后在途写凭证不提前释放。async_write 完成也不等于设备应用已确认收到 ACK。

idle 是合法完整请求的墙钟期限，因此耗时业务或本地暂停可能先遇到 idle；frame 才扣除暂停时间。Handler 没有被强制中止的能力，连接超时后执行中任务仍需有限时返回。SIGINT/SIGTERM 仍立即取消，没有优雅排空；该能力留到 M2.5。

本轮没有重新压测、没有新增性能目标。既有 experiments 数据和 performance 报告保持原样，其吞吐/P99 不能自动当成 M2.4 的性能结论。

## 风格与差异检查

按 AGENTS.md 审阅改动；19 个修改/新增的手写 C++ 文件通过项目 clang-format --dry-run --Werror，检查缩进、花括号及 100 列。Python 新增部分通过语法检查。使用本轮开始的文件快照做 git diff --no-index，新增文件单独审阅，确认无意外删除/无关格式化。

本地 .git 为只读且不是有效 Git 仓库，未改动它。GitHub 提交通过连接工具，以实际远端 main 为父提交并校验 expected_sha，不强制覆盖历史。文件范围与三个重点函数见 changes-m2.4.md。
