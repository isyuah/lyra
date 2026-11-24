import argparse
import os
from pathlib import Path
import shutil
from statistics import median
import subprocess
import tempfile


PACKAGE = Path(__file__).with_name('parallel-for')
EXPECTED = (148_933, 142_913_828_922)


def positive(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError('expected a positive integer')
    return number


def measure(executable, workers, warmups, runs, timeout, environment):
    result = subprocess.run(
        [executable], input='run\n' * (warmups + runs), capture_output=True,
        encoding='utf-8', errors='replace', timeout=timeout,
        env={**environment, 'LYRA_PARALLEL_WORKERS': str(workers)},
    )
    if result.returncode or result.stderr:
        raise ValueError(f'{workers} workers: exit {result.returncode}\n{result.stderr}')
    lines = result.stdout.splitlines()
    if len(lines) != warmups + runs + 1 or lines[0] != str(workers):
        raise ValueError(f'{workers} workers: unexpected benchmark output: {result.stdout!r}')

    samples = []
    for index, line in enumerate(lines[1:], start=1):
        fields = tuple(map(int, line.split()))
        if len(fields) != 3 or fields[0] <= 0 or fields[1:] != EXPECTED:
            raise ValueError(f'{workers} workers, run {index}: expected elapsed ns and {EXPECTED}, got {line!r}')
        samples.append(fields[0] / 1_000_000)
    return samples[warmups:]


def main():
    parser = argparse.ArgumentParser(description='Benchmark Lyra parallelFor on a fixed CPU workload.')
    parser.add_argument('compiler', type=Path, help='path to a built or installed Lyra compiler')
    parser.add_argument('--workers', nargs='+', type=positive, default=[1, 2, 4])
    parser.add_argument('--warmups', type=positive, default=2, help='untimed repetitions per worker count')
    parser.add_argument('--runs', type=positive, default=5, help='recorded repetitions per worker count')
    parser.add_argument('--timeout', type=positive, default=120, help='seconds allowed per build or worker count')
    options = parser.parse_args()
    if max(options.workers) > 64 or len(set(options.workers)) != len(options.workers):
        parser.error('--workers must contain distinct counts from 1 to 64')

    try:
        with tempfile.TemporaryDirectory(prefix='lyra-parallel-bench-') as temporary:
            root = Path(temporary)
            package = root / 'package'
            shutil.copytree(PACKAGE, package)
            executable = root / ('benchmark.exe' if os.name == 'nt' else 'benchmark')
            environment = {**os.environ, 'LYRA_CACHE': str(root / 'cache')}
            subprocess.run(
                [options.compiler.resolve(), 'build', package, '--release', '--offline', '-o', executable],
                env=environment, check=True, timeout=options.timeout,
            )
            results = {
                workers: measure(executable, workers, options.warmups, options.runs, options.timeout, environment)
                for workers in options.workers
            }
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f'benchmark failed: {error}\n')

    baseline = median(results[options.workers[0]])
    print(f'{EXPECTED[0]:,} primes; sum {EXPECTED[1]:,}; {options.warmups} warmups, {options.runs} measured runs')
    print(f'Workers  Median (ms)  Min (ms)  Max (ms)  Speedup vs {options.workers[0]}')
    for workers, samples in results.items():
        middle = median(samples)
        print(f'{workers:>7}  {middle:>11.3f}  {min(samples):>8.3f}  {max(samples):>8.3f}  {baseline / middle:>16.2f}x')


if __name__ == '__main__':
    main()
