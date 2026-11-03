"""Synthetic boundary fixtures for analysis; these are NOT performance observations."""
import gzip
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'benchmarks'))
import analyze


class BenchmarkAnalysis(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='gateway-analysis-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        # 第一条在预热期发出但在测量期收到；第三条在排空期收到。
        raw = ('connection,request_id,sent_ns,ack_ns,rtt_ns\n'
               '0,1,900000000,1100000000,200000000\n'
               '0,2,1200000000,1300000000,100000000\n'
               '0,3,1900000000,2400000000,500000000\n'
               '1,4,1400000000,1600000000,200000000\n')
        artifact = self.root / 'samples.csv.gz'
        artifact.write_bytes(gzip.compress(raw.encode(), mtime=0))
        self.summary = {
            'sample_scope': 'all_acknowledged', 'warmup_seconds': 1, 'measure_seconds': 1,
            'start_monotonic_ns': 0, 'measurement_samples': 3,
            'acknowledged_in_measure_interval': 3, 'acknowledged_including_warmup': 4,
            'throughput_ack_per_second': 3, 'p50_ns': 200000000,
            'p95_ns': 500000000, 'p99_ns': 500000000, 'max_ns': 500000000,
        }
        result = {'valid': True, 'business_result_log_lines': 0,
                  'artifacts': [{'path': artifact.name,
                                 'sha256': hashlib.sha256(artifact.read_bytes()).hexdigest()}]}
        (self.root / 'result.json').write_text(json.dumps(result))
        self.write_summary()
        (self.root / 'metadata.json').write_text(json.dumps(
            {'environment': {'clock_ticks_per_second': 100}}))
        samples = [{'monotonic_ns': tick, 'cpu_ticks': cpu, 'syscw': 0, 'wchar': 0,
                    'rss_bytes': 4096} for tick, cpu in [(1000000000, 10), (1900000000, 20)]]
        (self.root / 'process-samples.json').write_text(json.dumps({'server': samples}))

    def write_summary(self):
        (self.root / 'client-summary.json').write_text(json.dumps(self.summary))

    def test_warmup_ack_counts_for_throughput_and_drained_ack_counts_for_latency(self):
        result = analyze.analyze_run(self.root)
        self.assertEqual(result['measurement_samples'], 3)
        self.assertEqual(result['p99_ms'], 500)
        self.assertEqual(result['total_acknowledged'], 4)

    def test_rejects_throughput_count_that_disagrees_with_raw_samples(self):
        self.summary['acknowledged_in_measure_interval'] = 2
        self.write_summary()
        with self.assertRaisesRegex(ValueError, 'throughput ACK count mismatch'):
            analyze.analyze_run(self.root)

    def test_rejects_modified_artifact(self):
        with (self.root / 'samples.csv.gz').open('ab') as output:
            output.write(b'changed')
        with self.assertRaisesRegex(ValueError, 'artifact hash mismatch'):
            analyze.analyze_run(self.root)


if __name__ == '__main__':
    unittest.main()
