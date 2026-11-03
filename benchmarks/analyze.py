#!/usr/bin/env python3
"""Recompute nearest-rank quantiles from every saved request sample."""
import argparse
import csv
import gzip
import hashlib
import json
import math
from pathlib import Path
import statistics


def percentile(sorted_values, percent):
    return sorted_values[math.ceil(len(sorted_values) * percent / 100) - 1]


def process_metrics(samples, start, end, ticks):
    interval = [sample for sample in samples if start <= sample['monotonic_ns'] <= end]
    if len(interval) < 2:
        raise ValueError('not enough /proc samples in measurement interval')
    first, last = interval[0], interval[-1]
    seconds = (last['monotonic_ns'] - first['monotonic_ns']) / 1e9
    return {
        'sampled_seconds': seconds,
        'cpu_cores': (last['cpu_ticks'] - first['cpu_ticks']) / ticks / seconds,
        'write_syscalls_per_second': (last['syscw'] - first['syscw']) / seconds,
        'written_bytes_per_second': (last['wchar'] - first['wchar']) / seconds,
        'peak_sampled_rss_bytes': max(sample['rss_bytes'] for sample in interval),
    }


def analyze_run(root):
    result = json.loads((root / 'result.json').read_text())
    if not result['valid']:
        raise ValueError(f'invalid experiment {root}: {result["failure"]}')
    for artifact in result['artifacts']:
        digest = hashlib.sha256((root / artifact['path']).read_bytes()).hexdigest()
        if digest != artifact['sha256']:
            raise ValueError(f'artifact hash mismatch in {root}')
    summary = json.loads((root / 'client-summary.json').read_text())
    latency = []
    received_in_interval = 0
    raw_count = 0
    last_id = {}
    lo = summary['warmup_seconds'] * 10**9
    hi = lo + summary['measure_seconds'] * 10**9
    with gzip.open(root / 'samples.csv.gz', 'rt') as source:
        for row in csv.DictReader(source):
            connection, rid = int(row['connection']), int(row['request_id'])
            sent, ack, rtt = int(row['sent_ns']), int(row['ack_ns']), int(row['rtt_ns'])
            raw_count += 1
            if not (0 <= sent < hi and ack >= sent and ack - sent == rtt):
                raise ValueError(f'inconsistent timestamp in {root}')
            if rid <= last_id.get(connection, 0):
                raise ValueError(f'duplicate or reordered request_id in {root}')
            last_id[connection] = rid
            if lo <= sent < hi:
                latency.append(rtt)
            if lo <= ack < hi:
                received_in_interval += 1
    if summary.get('sample_scope') == 'all_acknowledged':
        if received_in_interval != summary['acknowledged_in_measure_interval']:
            raise ValueError(f'throughput ACK count mismatch in {root}')
        if raw_count != summary['acknowledged_including_warmup']:
            raise ValueError(f'total ACK count mismatch in {root}')
    latency.sort()
    for percent in (50, 95, 99):
        if percentile(latency, percent) != summary[f'p{percent}_ns']:
            raise ValueError(f'quantile mismatch in {root}')
    if len(latency) != summary['measurement_samples']:
        raise ValueError(f'sample count mismatch in {root}')
    metadata = json.loads((root / 'metadata.json').read_text())
    processes = json.loads((root / 'process-samples.json').read_text())
    start = summary['start_monotonic_ns']
    ticks = metadata['environment'].get('clock_ticks_per_second', 100)
    resources = {role: process_metrics(samples, start + lo, start + hi, ticks)
                 for role, samples in processes.items()}
    return {
        'run': root.name,
        'throughput_ack_per_second': summary['throughput_ack_per_second'],
        'p50_ms': summary['p50_ns'] / 1e6,
        'p95_ms': summary['p95_ns'] / 1e6,
        'p99_ms': summary['p99_ns'] / 1e6,
        'max_ms': summary['max_ns'] / 1e6,
        'measurement_samples': len(latency),
        'total_acknowledged': summary['acknowledged_including_warmup'],
        'business_result_log_lines': result['business_result_log_lines'],
        'resources': resources,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    args = parser.parse_args()
    runs = [analyze_run(path) for path in sorted(args.root.iterdir())
            if path.is_dir() and (path / 'result.json').exists()]
    groups = {}
    # Pilot is retained as evidence of the initial hypothesis, excluded from formal comparisons.
    for prefix in ('baseline', 'optimized', 'logging-control'):
        members = [run for run in runs if run['run'].startswith(prefix + '-')]
        if not members:
            continue
        groups[prefix] = {
            'runs': len(members),
            'median_throughput_ack_per_second': statistics.median(
                run['throughput_ack_per_second'] for run in members),
            'median_p99_ms': statistics.median(run['p99_ms'] for run in members),
            'throughput_range': [min(run['throughput_ack_per_second'] for run in members),
                                 max(run['throughput_ack_per_second'] for run in members)],
            'p99_ms_range': [min(run['p99_ms'] for run in members),
                            max(run['p99_ms'] for run in members)],
        }
    output = {'runs': runs, 'groups': groups}
    (args.root / 'analysis.json').write_text(json.dumps(output, indent=2) + '\n')
    print(json.dumps(output, indent=2))


if __name__ == '__main__':
    raise SystemExit(main())
