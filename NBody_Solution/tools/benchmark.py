#!/usr/bin/env python3
# Повторные запуски C-программ, сравнение траекторий и сводка времени.
import argparse
import csv
from itertools import zip_longest
import math
from pathlib import Path
import random
import re
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]


# Сравниваем время и координаты с допуском на округление double.
def compare(reference, candidate):
    max_error = 0.0
    max_scaled_error = 0.0
    value_count = 0

    with reference.open() as reference_file, candidate.open() as candidate_file:
        reference_rows = csv.reader(reference_file)
        candidate_rows = csv.reader(candidate_file)
        if next(reference_rows) != next(candidate_rows):
            raise AssertionError('Different CSV headers')

        # zip_longest позволяет заметить лишнюю строку в любом из файлов.
        for reference_row, candidate_row in zip_longest(reference_rows, candidate_rows):
            if reference_row is None or candidate_row is None:
                raise AssertionError('Different CSV sizes')
            if len(reference_row) != len(candidate_row):
                raise AssertionError('Different CSV sizes')

            reference_values = map(float, reference_row)
            candidate_values = map(float, candidate_row)
            for expected, actual in zip(reference_values, candidate_values):
                if not math.isfinite(expected) or not math.isfinite(actual):
                    raise AssertionError('Non-finite value')

                error = abs(expected - actual)
                max_error = max(max_error, error)
                max_scaled_error = max(max_scaled_error, error / (1 + abs(expected)))
                tolerance = 1e-10 + 1e-10 * abs(expected)
                if error > tolerance:
                    raise AssertionError(f'Mismatch: {expected} vs {actual}')
                value_count += 1

    return max_error, max_scaled_error, value_count


# Фиксированное начальное значение генератора даёт одинаковые входы для всех прогонов.
def generate(n, path):
    rng = random.Random(20261015 + n)
    with path.open('w') as file:
        file.write(f'{n}\n')
        for i in range(n):
            # Тела размещены на сетке, поэтому начальные положения не совпадают.
            body = [
                rng.uniform(1e10, 1e12),
                1000.0 * (i % 32),
                1000.0 * ((i // 32) % 32),
                1000.0 * (i // 1024),
            ]
            body += [rng.uniform(-1, 1) for _ in range(3)]
            file.write(' '.join(format(value, '.17g') for value in body) + '\n')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', default='measurements_local')
    parser.add_argument('--sizes', nargs='+', type=int, default=[128, 2048])
    parser.add_argument('--threads', nargs='+', type=int, default=[1, 2, 4, 8])
    parser.add_argument('--repeats', type=int, default=5)
    args = parser.parse_args()
    if args.repeats < 2 or min(args.sizes) < 1 or min(args.threads) < 1:
        parser.error('Use positive sizes/threads and at least two repetitions')

    output_dir = Path(args.out).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    raw_results = []
    comparison_results = []

    configurations = [('serial', 1)]
    for threads in args.threads:
        configurations.append(('pthreads', threads))
    order_rng = random.Random(71)

    for n in args.sizes:
        input_path = output_dir / f'input_{n}.txt'
        generate(n, input_path)
        reference_path = output_dir / f'trajectory_{n}_serial.csv'

        # Нулевой раунд — прогрев. Последовательный вариант создаёт первый эталон.
        for round_no in range(args.repeats + 1):
            order = configurations[:]
            if round_no > 0:
                order_rng.shuffle(order)

            for mode, threads in order:
                trajectory_path = output_dir / f'trajectory_{n}_{mode}_{threads}.csv'
                command = [
                    str(ROOT / f'nbody_{mode}'), '1', str(input_path),
                    '0.0625', str(trajectory_path),
                ]
                if mode == 'pthreads':
                    command.append(str(threads))
                command.append('xyz')

                # Время уже измерено внутри C-программы; читаем его из её вывода.
                result = subprocess.run(
                    command, capture_output=True, text=True, timeout=300, check=True,
                )
                compute_match = re.search(r'Compute time[^=]*=\s*(\S+)', result.stdout)
                total_match = re.search(r'Total time[^=]*=\s*(\S+)', result.stdout)
                compute_time = float(compute_match[1])
                total_time = float(total_match[1])

                if mode == 'serial':
                    reference_path.write_bytes(trajectory_path.read_bytes())

                # Прогрев не включаем в результаты. Для serial сравнение служебное: с его копией.
                if round_no > 0:
                    error, scaled_error, cells = compare(reference_path, trajectory_path)
                    comparison_results.append([
                        n, mode, threads, round_no, error, scaled_error, cells, 'PASS',
                    ])
                    raw_results.append([
                        n, mode, threads, round_no, compute_time, total_time,
                    ])
                print(
                    f'N={n} {mode} P={threads} round={round_no} compute={compute_time:.6f}s',
                    flush=True,
                )

    # Сохраняем отдельные измерения до усреднения, чтобы расчёты можно было проверить.
    with (output_dir / 'raw_times.csv').open('w', newline='') as file:
        writer = csv.writer(file)
        writer.writerow(['N', 'mode', 'P', 'repeat', 'compute_seconds', 'total_seconds'])
        writer.writerows(raw_results)

    with (output_dir / 'comparisons.csv').open('w', newline='') as file:
        writer = csv.writer(file)
        writer.writerow([
            'N', 'mode', 'P', 'repeat', 'max_abs', 'max_scaled', 'cells', 'status',
        ])
        writer.writerows(comparison_results)

    # Для ускорения берём среднее вычислительное время последовательной версии.
    summary = []
    for n in args.sizes:
        serial_times = [
            row[4] for row in raw_results if row[0] == n and row[1] == 'serial'
        ]
        serial_mean = statistics.mean(serial_times)
        for mode, threads in configurations:
            records = [
                row for row in raw_results if row[:3] == [n, mode, threads]
            ]
            compute_times = [row[4] for row in records]
            total_times = [row[5] for row in records]
            compute_mean = statistics.mean(compute_times)
            speedup = serial_mean / compute_mean
            efficiency = speedup / threads
            summary.append([
                n, mode, threads, compute_mean, statistics.stdev(compute_times),
                min(compute_times), max(compute_times), statistics.mean(total_times),
                speedup, efficiency,
            ])

    with (output_dir / 'summary.csv').open('w', newline='') as file:
        writer = csv.writer(file)
        writer.writerow([
            'N', 'mode', 'P', 'compute_mean', 'compute_stdev', 'compute_min',
            'compute_max', 'total_mean', 'speedup', 'efficiency',
        ])
        writer.writerows(summary)
    print(f'All comparisons PASS; results: {output_dir}', flush=True)


if __name__ == '__main__':
    main()
