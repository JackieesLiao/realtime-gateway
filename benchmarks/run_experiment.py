#!/usr/bin/env python3
"""Run one measured Linux loopback experiment; preserve successes AND failures."""
import argparse
import datetime
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import time


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1048576), b''):
            digest.update(block)
    return digest.hexdigest()


def command_output(command):
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    return {'command': command, 'returncode': result.returncode,
            'stdout': result.stdout, 'stderr': result.stderr}


def read_text(path):
    try:
        return Path(path).read_text().strip()
    except OSError as error:
        return str(error)


def environment():
    return {
        'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'uname': platform.uname()._asdict(),
        'os_release': read_text('/etc/os-release'),
        'memory': read_text('/proc/meminfo'),
        'cpu_topology': command_output(['lscpu', '-p=CPU,CORE,SOCKET']),
        'cpu_model': command_output(['lscpu']),
        'available_cpus': sorted(os.sched_getaffinity(0)),
        'clock_ticks_per_second': os.sysconf('SC_CLK_TCK'),
        'cgroup': read_text('/proc/self/cgroup'),
        'cgroup_cpu_max': read_text('/sys/fs/cgroup/cpu.max'),
        'cgroup_memory_max': read_text('/sys/fs/cgroup/memory.max'),
        'cpu_pressure': read_text('/proc/pressure/cpu'),
        'loadavg': read_text('/proc/loadavg'),
        'compiler': command_output(['/usr/bin/g++', '--version']),
        'protoc': command_output(['/usr/bin/protoc', '--version']),
        'packages': command_output(['dpkg-query', '-W', 'libboost1.83-dev',
                                    'libprotobuf-dev', 'libspdlog-dev', 'nlohmann-json3-dev']),
    }


def sample_process(pid):
    try:
        # comm may contain spaces or ')'; fields after its last ')' start at stat field 3.
        tail = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        io = {}
        for line in Path(f'/proc/{pid}/io').read_text().splitlines():
            key, value = line.split(':')
            io[key] = int(value)
        return {'monotonic_ns': time.monotonic_ns(),
                'cpu_ticks': int(tail[11]) + int(tail[12]),
                'user_ticks': int(tail[11]), 'system_ticks': int(tail[12]),
                'rss_bytes': int(tail[21]) * os.sysconf('SC_PAGE_SIZE'), **io}
    except (OSError, ValueError):
        return None


def compress(path):
    compressed = path.with_suffix(path.suffix + '.gz')
    # Compression happens AFTER the measured processes stop; mtime=0 makes it deterministic.
    with path.open('rb') as source, compressed.open('wb') as raw:
        with gzip.GzipFile(filename='', fileobj=raw, mode='wb', mtime=0) as destination:
            for block in iter(lambda: source.read(1048576), b''):
                destination.write(block)
    path.unlink()
    return {'path': compressed.name, 'sha256': sha256(compressed),
            'bytes': compressed.stat().st_size}


def stop_process(process):
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
    try:
        return process.wait(timeout=8)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        raise RuntimeError('process did not stop within 8 seconds')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--client', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--config', type=Path, default=Path('config/gateway.json'))
    parser.add_argument('--server-cpus', default='0,2,4,6')
    parser.add_argument('--client-cpus', default='8,10,12,14,16,18,20,22')
    parser.add_argument('--connections', type=int, default=16)
    parser.add_argument('--window', type=int, default=8)
    parser.add_argument('--payload', type=int, default=256)
    parser.add_argument('--warmup', type=int, default=2)
    parser.add_argument('--seconds', type=int, default=6)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    root = args.out.resolve()
    config = json.loads(args.config.read_text())
    config.update(listen_address='127.0.0.1', port=0)
    config_path = root / 'server-config.json'
    config_path.write_text(json.dumps(config, indent=4) + '\n')
    server_command = ['taskset', '-c', args.server_cpus,
                      str(args.server.resolve()), str(config_path)]
    metadata = {'environment': environment(), 'server_command': server_command,
                'server_sha256': sha256(args.server), 'client_sha256': sha256(args.client),
                'baseline_source_commit': '719942d20fd52c6150959ffde040175d49488437',
                'arguments': {key: str(value) for key, value in vars(args).items()}}
    (root / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    samples = {'server': [], 'client': []}
    client = None
    failure = None
    log_path = root / 'server.jsonl'
    with log_path.open('w') as log:
        server = subprocess.Popen(server_command, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 8
            while True:
                with log_path.open() as source:
                    events = [json.loads(line) for line in source if line.startswith('{')]
                listening = next((event for event in events
                                  if event.get('event') == 'listening'), None)
                if listening:
                    break
                if server.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('server failed to become ready')
                time.sleep(0.01)
            client_command = ['taskset', '-c', args.client_cpus, str(args.client.resolve()),
                              str(listening['port']), str(args.connections), str(args.window),
                              str(args.payload), str(args.warmup), str(args.seconds),
                              str(root / 'samples.csv'), '2000000']
            metadata['client_command'] = client_command
            (root / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
            with (root / 'client-summary.json').open('w') as output:
                with (root / 'client.stderr').open('w') as errors:
                    client = subprocess.Popen(client_command, stdout=output, stderr=errors)
                    deadline = time.monotonic() + args.warmup + args.seconds + 15
                    while client.poll() is None:
                        for role, process in [('server', server), ('client', client)]:
                            sample = sample_process(process.pid)
                            if sample is not None:
                                samples[role].append(sample)
                        if time.monotonic() >= deadline:
                            raise RuntimeError('benchmark process deadline exceeded')
                        time.sleep(0.1)
            if client.returncode != 0:
                raise RuntimeError(f'client failed with {client.returncode}')
        except Exception as error:
            failure = str(error)
        finally:
            if client is not None and client.poll() is None:
                client.kill()
                client.wait()
            try:
                server_returncode = stop_process(server)
                if server_returncode != 0:
                    failure = failure or f'server failed with {server_returncode}'
            except RuntimeError as error:
                failure = failure or str(error)
    (root / 'process-samples.json').write_text(json.dumps(samples, indent=2) + '\n')
    statistics = None
    business_logs = 0
    with log_path.open() as source:
        for line in source:
            if not line.startswith('{'):
                continue
            event = json.loads(line)
            if event.get('event') == 'statistics':
                statistics = event
            if event.get('event') == 'business_result':
                business_logs += 1
    (root / 'server-statistics.json').write_text(json.dumps(statistics, indent=2) + '\n')
    if statistics is None:
        failure = failure or 'missing server statistics'
    else:
        zero_fields = ['business_outstanding', 'business_outstanding_bytes', 'business_failed',
                       'completion_failed', 'budget_messages', 'budget_bytes', 'budget_waiters',
                       'budget_pending_wakeups', 'rejected']
        if any(statistics[field] != 0 for field in zero_fields):
            failure = failure or 'server failure or unreturned resource budget'
        summary_path = root / 'client-summary.json'
        if summary_path.exists() and summary_path.stat().st_size:
            summary = json.loads(summary_path.read_text())
            if statistics['processed'] != summary['acknowledged_including_warmup']:
                failure = failure or 'processed/ACK mismatch'
    artifacts = [compress(log_path)]
    if (root / 'samples.csv').exists():
        artifacts.append(compress(root / 'samples.csv'))
    result = {'valid': failure is None, 'failure': failure,
              'business_result_log_lines': business_logs,
              'artifacts': artifacts}
    (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'directory': str(root), **result}))
    return int(failure is not None)


if __name__ == '__main__':
    raise SystemExit(main())
