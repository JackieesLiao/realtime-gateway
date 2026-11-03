#!/usr/bin/env python3
"""Run baseline/optimized/logging-control in rotating order, without parallel benchmarks."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--client', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--common-config', type=Path, required=True)
    parser.add_argument('--control-config', type=Path, required=True)
    parser.add_argument('--server-cpus', default='0,2,4,6')
    parser.add_argument('--client-cpus', default='8,10,12,14,16,18,20,22')
    parser.add_argument('--seconds', type=int, default=6)
    parser.add_argument('--warmup', type=int, default=2)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    plan_path = args.out / 'plan.json'
    if plan_path.exists():
        raise ValueError('plan already exists; use a fresh output directory')
    variants = [
        ('baseline', args.baseline, args.common_config),
        ('optimized', args.candidate, args.common_config),
        ('logging-control', args.candidate, args.control_config),
    ]
    jobs = []
    for repetition in range(3):
        for variant, server, config in variants[repetition:] + variants[:repetition]:
            command = [sys.executable, str(Path(__file__).with_name('run_experiment.py')),
                       '--server', str(server), '--client', str(args.client),
                       '--config', str(config),
                       '--out', str(args.out / f'{variant}-{repetition + 1}'),
                       '--server-cpus', args.server_cpus, '--client-cpus', args.client_cpus,
                       '--connections', '16', '--window', '8', '--payload', '256',
                       '--warmup', str(args.warmup), '--seconds', str(args.seconds)]
            jobs.append({'variant': variant, 'repetition': repetition + 1, 'command': command})
    # 运行前固定并保存全部顺序与参数；失败也保留记录，不挑选好看的成功轮次。
    plan_path.write_text(json.dumps(jobs, indent=2) + '\n')
    with (args.out / 'commands.sh').open('w') as output:
        output.write('# Commands executed in order; use fresh --out paths when reproducing.\n')
        for job in jobs:
            output.write(shlex.join(job['command']) + '\n')
    for job in jobs:
        print(f'Running {job["variant"]} round {job["repetition"]}', flush=True)
        result = subprocess.run(job['command'], check=False)
        job['returncode'] = result.returncode
        plan_path.write_text(json.dumps(jobs, indent=2) + '\n')
        if result.returncode:
            return result.returncode
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
