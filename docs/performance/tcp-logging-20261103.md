# TCP 日志热路径对照实验

本次在复刻仓库亲自执行基线、定位假设、代码调整及相同负载复测。它是经用户授权提前开展的一次 M4 范围实验；M2.4/M2.5 及 M4 其余功能尚未完成。数据来自 WSL2 loopback，不代表原公司部署环境或公网性能。

## 结果先看

三组各三轮，下面使用“每轮指标的中位数”，没有把不同轮次的 P99 平均或混成一个分位数：

| 版本 | 吞吐（ACK/s） | P99（ms） | 三轮吞吐范围 | 三轮 P99 范围（ms） |
| --- | ---: | ---: | --- | --- |
| 基线：每条成功请求打印 info JSON | 24,235.17 | 7.595808 | 23,726.50–24,536.83 | 7.459758–7.710274 |
| 优化：成功日志为 debug，默认关闭 | 34,822.83 | 5.410793 | 34,518.67–34,975.17 | 5.229614–5.498317 |
| 对照：同一个优化二进制，重新开启 debug | 24,356.17 | 7.519320 | 23,984.83–24,607.33 | 7.483198–7.726706 |

相对基线，优化组吞吐中位数提高 43.69%，P99 中位数降低 28.77%。全部正式轮次共收到 1,998,773 个正确 ACK（含预热及排空）；请求与 ACK 一一匹配，服务端 processed 与 ACK 总数一致，业务错误、完成回送错误和退出预算残留均为 0。

这证明在本次配置和日志策略下，成功请求日志热路径具有可测的成本。它不证明日志是唯一瓶颈，也没有分别测出 JSON 构造、格式化、锁和写入各自占比；没有生成火焰图或声称做过 CPU 栈采样。

## 环境与负载

- WSL2 Ubuntu 24.04.4，内核 6.6.87.2-microsoft-standard-WSL2，x86_64。
- CPU 型号 Ryzen 9 8945HX；Linux 可见 32 个逻辑 CPU、16 个物理核心，内存可见 16,391,467,008 字节，swap 4 GiB。没有读取 Windows 主机资源配置；cgroup 根目录的 cpu.max/memory.max 不可用，原始读取结果保留在 metadata.json。
- GCC 13.3.0，C++20，Release `-O3 -DNDEBUG`，无 sanitizer。Boost 1.83、protobuf 3.21.12、spdlog 1.12；apt 版本及完整编译命令已保存。
- 服务端 2 个 I/O 线程、2 个业务线程，绑定逻辑 CPU 0,2,4,6（四个不同物理核心）。客户端绑定 8,10,12,14,16,18,20,22（另外八个物理核心）。CPU 号来自实际 lscpu 拓扑；复现时需按自己的机器修改。
- 同机 127.0.0.1 TCP；客户端每连接一个线程，16 个长连接，每连接最多 8 条在途请求，共最多 128 条。payload 固定 256 字节，业务为现有 TELEMETRY 校验和统计，没有人工 sleep、重试或持久化。
- 客户端 TCP_NODELAY 始终开启，服务端 socket 选项保持原样。请求按当前 framing/protobuf 生成，逐条检查 ACK 长度、protobuf、request_id、OK 及空 error。
- 每轮新服务进程和连接；所有客户端线程 barrier 就绪后统一计时。预热 2 秒、测量 6 秒，随后停止发新请求并收完在途 ACK。连接建立、CSV 写入、压缩和分析不在测量窗口内。
- 服务端 stdout 始终重定向到同一 Linux 文件系统的普通文件，保留原 spdlog 同步输出行为。没有 fsync 或磁盘吞吐测试，写入可能由页缓存承接，不能把结果归因于物理磁盘饱和。

业务与预算配置见 [common-config.json](../../experiments/tcp-20261103/common-config.json)。对照配置只增加 `log_level=debug`，见 [debug-config.json](../../experiments/tcp-20261103/debug-config.json)。各轮使用 port=0，实际端口记录在服务端命令/日志中。

## 测量定义

这是固定并发窗口的闭环负载：收到一条 ACK 后补一条请求。吞吐定义为测量时间区间内收到并校验成功的 ACK 数除以 6 秒，不把预热或排空时间加入分母，也不把发送次数当成功吞吐。

延迟使用 steady_clock，单位纳秒，从开始写该请求到完整 ACK 收到并校验完成，包含客户端写等待、服务端排队、业务与网络往返，但不包含请求序列化前的客户端工作。只选择在测量区间发出的请求算分位数；它们在截止后才返回也计入。P99 使用 nearest-rank（排序后第 ceil(N*0.99) 个）。

吞吐按接收时间选区间，延迟按发送时间选 cohort；两者样本数可相差最多在途窗口的量，不应强行要求相同。正式 raw CSV 保存所有 ACK，包括预热及排空，可以独立重算两种指标。分析器校验哈希、时间戳、request_id 单调性、样本数、吞吐计数和 P50/P95/P99。

闭环会随服务变慢自动降低发送速度，存在 coordinated omission 的局限：本报告不能用于宣称固定外部到达率下的 P99/SLO。相同连接数/窗口代表相同并发负载，不代表三个版本发送数量相等。后续仍需补定速到达、超载错误率、更多 payload 和并发梯度。

## 从数据到调整

初次 1 秒预热、3 秒测量的基线试跑约 24,362 ACK/s、P99 7.537473 ms；采样约 24,316 次写系统调用/s、约 2.19 MB/s 日志输出，逐请求日志共 96,593 条。检查代码发现 Session::handle_business_result 在连接 strand 内为每条成功请求构造 JSON、转换枚举名并同步调用 spdlog。

由此提出假设：成功日志的构造和同步输出正在消耗每个请求的处理时间。没有直接断言 CPU 已满或磁盘已满。

实现只调整日志策略：成功结果降为 debug，默认 info 关闭；在构造 JSON 之前先检查 should_log。业务失败仍记录 info，连接生命周期与汇总统计仍为 info。新增 log_level 配置只允许 info/debug。没有修改 Router、队列容量、预算、线程数、ACK 或 socket 设置。

正式实验三组各三轮，以如下顺序执行，避免所有旧版先跑、新版后跑：

```text
baseline-1 → optimized-1 → logging-control-1
optimized-2 → logging-control-2 → baseline-2
logging-control-3 → baseline-3 → optimized-3
```

各轮具体结果：

| 轮次 | ACK/s | P99 ms | 服务端采样 CPU 核数 | 客户端采样 CPU 核数 |
| --- | ---: | ---: | ---: | ---: |
| baseline-1 | 24,235.17 | 7.595808 | 1.871 | 0.854 |
| baseline-2 | 24,536.83 | 7.459758 | 1.895 | 0.886 |
| baseline-3 | 23,726.50 | 7.710274 | 1.880 | 0.853 |
| optimized-1 | 34,975.17 | 5.229614 | 1.900 | 1.251 |
| optimized-2 | 34,518.67 | 5.410793 | 1.861 | 1.214 |
| optimized-3 | 34,822.83 | 5.498317 | 1.887 | 1.228 |
| logging-control-1 | 24,356.17 | 7.519320 | 1.893 | 0.876 |
| logging-control-2 | 24,607.33 | 7.483198 | 1.909 | 0.892 |
| logging-control-3 | 23,984.83 | 7.726706 | 1.866 | 0.876 |

CPU 来自约 100 ms 一次的 /proc/<pid>/stat 采样，1.0 表示累计使用一个核，不是整机 100%。只取测量区间内的首尾样本做差，实际采样跨度也保留；它是辅助证据，不是精确每请求 profiling。

基线、优化和对照的服务端 CPU 中位数约 1.88、1.89、1.89 核；优化并未靠多用大量 CPU 换吞吐。基线写系统调用中位数约 24,235/s，对照约 24,362/s，优化在测量期间为 0；生命周期日志仍发生于区间之外。客户端 CPU 约 0.85 → 1.23 核，未用满分配的八个物理核心，但本实验没有据此宣称完全排除客户端瓶颈。

最有力的证据是对照组：同一个优化二进制重新开日志，逐请求日志数量恢复为 ACK 总数，吞吐及 P99 回到基线范围；关闭后连续三轮表现均改善。因此判断针对的是“成功日志热路径整体”，不是根据两个偶然数字推断。

代价明确：默认不再保留每条成功请求明细。需要逐请求诊断时可打开 debug，但需接受相应成本。并未实现无损异步日志或宣称在保持相同明细输出量时也能获得上述收益。

## 命令、原始文件与复现

原始文件都在 [experiments/tcp-20261103](../../experiments/tcp-20261103)：

- plan.json：运行前固定顺序，运行后保存每次退出码；commands.sh：实际执行的九条命令。
- 每轮 metadata.json：实际命令、UTC、系统/依赖版本、CPU 拓扑、二进制 SHA256。
- source-manifest.json：基线提交、源码哈希、基线/候选/客户端二进制哈希及实际编译命令。基线是 [719942d](https://github.com/JackieesLiao/realtime-gateway/commit/719942d20fd52c6150959ffde040175d49488437) 的 Release 二进制，在修改生产代码前复制保存。
- samples.csv.gz：完整逐请求 connection/request_id/sent_ns/ack_ns/rtt_ns；正式组含全部预热和排空样本。
- server.jsonl.gz：完整服务日志；server-statistics.json：退出业务和预算计数。
- process-samples.json：原始 /proc CPU、RSS、I/O 计数；client-summary.json：客户端统计。
- result.json：有效性、失败原因（若有）、日志行数、压缩文件 SHA256；analysis.json：独立重算及组间汇总。

pilot-baseline 与 pilot-optimized 是探索记录，均不参与正式中位数。试跑客户端仅保存测量 cohort；正式客户端已扩展为保存全部 ACK。pilot-optimized 曾与一次 Release 功能回归短暂并行，因此更不能作为正式比较依据；九轮正式测量期间没有并行编译或测试。探索记录保留，不挑选或删除不利数据。

本次执行的核心命令（基线复制发生于日志代码修改之前）：

```bash
cmake --preset release
cmake --build --preset release
cp build/release/gateway_server /tmp/gateway-perf-baseline-server
# 完成日志调整并再次构建 Release 后：
python3 benchmarks/run_matrix.py \
  --baseline /tmp/gateway-perf-baseline-server \
  --candidate build/release/gateway_server \
  --client build/release/gateway_bench \
  --common-config experiments/tcp-20261103/common-config.json \
  --control-config experiments/tcp-20261103/debug-config.json \
  --out experiments/tcp-20261103
python3 benchmarks/analyze.py experiments/tcp-20261103
```

从 GitHub 干净检出后，先生成旧版本二进制，再使用新目录复测；不要覆盖这次的原始记录：

```bash
git worktree add --detach /tmp/realtime-gateway-baseline 719942d
cmake -S /tmp/realtime-gateway-baseline \
  -B /tmp/realtime-gateway-baseline/build/release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DProtobuf_PROTOC_EXECUTABLE=/usr/bin/protoc -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build /tmp/realtime-gateway-baseline/build/release -j2
cmake --preset release
cmake --build --preset release
python3 benchmarks/run_matrix.py \
  --baseline /tmp/realtime-gateway-baseline/build/release/gateway_server \
  --candidate build/release/gateway_server --client build/release/gateway_bench \
  --common-config experiments/tcp-20261103/common-config.json \
  --control-config experiments/tcp-20261103/debug-config.json \
  --out experiments/reproduce-new
python3 benchmarks/analyze.py experiments/reproduce-new
```

所有矩阵负载参数固定于 run_matrix.py；支持通过 --server-cpus/--client-cpus 改亲和性，修改后应明确是另一环境。run_experiment.py 可用于单轮、更改连接/窗口/payload 的后续实验，已有目录拒绝覆盖。没有自动安装依赖、修改系统配置或调整内核网络参数。

## 回归、范围与阅读函数

Debug、Release、ASan/UBSan 和 TSan（setarch x86_64 -R）构建/测试命令沿用 presets。新增日志策略 socket 测试验证默认成功明细关闭、失败明细保留、debug 恢复；新增分析器边界测试覆盖预热跨界、排空长尾以及样本损坏。实际检查记录见本目录的验证说明。

生产修改仅限 Config、main 日志等级、log helper 和 Session 成功日志路径；新增 benchmarks 客户端/运行矩阵/分析工具、测试、CMake 登记和实验资料。原协议与背压没有变化，没有提前补齐整个 M4。

三个重点函数：

1. run_connection（benchmarks/gateway_bench.cpp）：有界闭环窗口、ACK 校验、完整采样与截止后的排空。
2. Session::handle_business_result（src/server.cpp）：构造 JSON 前检查等级，保留失败日志，业务和预算交接保持原样。
3. analyze_run（benchmarks/analyze.py）：从原始样本校验并重算吞吐和分位数，避免只相信客户端打印的汇总。

本次只做了三轮短时重复。Windows 其他负载、WSL 调度、温度和后台活动不能完全控制，也没有跨机器、跨主机网络或长时间稳定性验证。不能将这些数字当生产容量、其他协议性能或固定到达率下的延迟保证。
