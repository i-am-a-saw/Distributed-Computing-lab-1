# Проверки физических формул, шагов Эйлера и распределения работы между потоками.
import csv
import math
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
G = 6.67430e-11


def reference(bodies, end, dt):
    # Эталон не использует C-код: сила для каждого тела считается отдельно.
    # Поэтому одна пара тел здесь учитывается дважды, по одному разу для каждого тела.
    bodies = [body[:] for body in bodies]
    time = 0.0
    positions = [value for body in bodies for value in body[1:4]]
    rows = [[time] + positions]
    while time < end:
        # Время задаётся номером шага, а не суммой уже сделанных шагов.
        next_time = len(rows) * dt
        if next_time >= end or end - next_time <= 4 * sys.float_info.epsilon * end:
            next_time = end
        step = next_time - time
        updated_bodies = []
        for body_index, body in enumerate(bodies):
            acceleration = [0.0] * 3
            for other_index, other in enumerate(bodies):
                if body_index == other_index:
                    continue
                delta = [other[axis] - body[axis] for axis in range(1, 4)]
                distance = math.sqrt(sum(component * component for component in delta))
                for axis in range(3):
                    acceleration[axis] += G * other[0] * delta[axis] / distance**3

            # Все тела обновляются по старым координатам и скоростям.
            # Новое состояние сохраняется отдельно до завершения шага.
            new_position = [body[axis + 1] + step * body[axis + 4] for axis in range(3)]
            new_velocity = [body[axis + 4] + step * acceleration[axis] for axis in range(3)]
            updated_bodies.append([body[0]] + new_position + new_velocity)
        bodies = updated_bodies
        time = next_time
        positions = [value for body in bodies for value in body[1:4]]
        rows.append([time] + positions)
    return rows


class NbodyTests(unittest.TestCase):
    def run_program(self, bodies, end=0.07, dt=0.01, threads=None, fmt='xyz'):
        # Вход и результат каждого запуска изолированы во временной папке.
        with tempfile.TemporaryDirectory() as directory:
            input_path = Path(directory) / 'input.txt'
            output_path = Path(directory) / 'output.csv'
            body_lines = [' '.join(map(str, body)) for body in bodies]
            input_path.write_text(str(len(bodies)) + '\n' + '\n'.join(body_lines) + '\n')
            executable = 'nbody_serial' if threads is None else 'nbody_pthreads'
            command = [str(ROOT / executable), str(end), str(input_path),
                       str(dt), str(output_path)]
            if threads is not None:
                command.append(str(threads))
            result = subprocess.run(command + [fmt], capture_output=True, text=True,
                                    timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            with output_path.open() as file:
                reader = csv.reader(file)
                header = next(reader)
                rows = [list(map(float, row)) for row in reader]
            return header, rows

    def assert_rows(self, actual, expected):
        self.assertEqual(len(actual), len(expected))
        # Небольшой допуск учитывает округление чисел с плавающей точкой.
        for actual_row, expected_row in zip(actual, expected):
            self.assertEqual(len(actual_row), len(expected_row))
            for actual_value, expected_value in zip(actual_row, expected_row):
                self.assertTrue(math.isclose(actual_value, expected_value,
                                             rel_tol=2e-12, abs_tol=1e-12),
                                (actual_value, expected_value))

    def test_one_body_linear_3d(self):
        # Без других тел скорость постоянна, а последний шаг может быть короче dt.
        bodies = [[2, 1, -2, 3, 4, -5, 6]]
        for threads in (None, 1, 2, 4):
            _, rows = self.run_program(bodies, end=0.025, threads=threads)
            self.assert_rows(rows, reference(bodies, 0.025, 0.01))
            self.assertEqual(rows[-1][0], 0.025)

    def test_two_body_old_velocity_and_force_sign(self):
        bodies = [[1e12, -10, 0, 0, 0, 0, 0],
                  [2e12, 10, 0, 0, 0, 0, 0]]
        for threads in (None, 1, 2, 4):
            _, rows = self.run_program(bodies, end=2, dt=1, threads=threads)
            # При нулевой начальной скорости первый шаг не меняет координаты.
            self.assertEqual(rows[1][1:], rows[0][1:])
            self.assertAlmostEqual(rows[2][1], -10 + G * 2e12 / 400)
            self.assertAlmostEqual(rows[2][4], 10 - G * 1e12 / 400)
            # Внутренние силы не должны сдвигать центр масс системы.
            for row in rows:
                self.assertAlmostEqual((row[1] + 2 * row[4]) / 3, 10 / 3)

    def test_random_3d_and_nondivisible_work(self):
        rng = random.Random(19)
        for n in (2, 5, 11):
            bodies = []
            for _ in range(n):
                mass = rng.uniform(1e10, 1e12)
                position = [rng.uniform(-100, 100) for _ in range(3)]
                velocity = [rng.uniform(-1, 1) for _ in range(3)]
                bodies.append([mass] + position + velocity)
            expected = reference(bodies, 0.07, 0.01)
            # Проверяем и неравные участки работы, и число потоков больше числа тел.
            for threads in (None, 1, 2, 3, 8):
                with self.subTest(n=n, threads=threads):
                    _, rows = self.run_program(bodies, threads=threads)
                    self.assert_rows(rows, expected)

    def test_time_grid_without_extra_step(self):
        # Число шагов задано явно и не зависит от алгоритма эталона.
        cases = [
            (10, 0.01, 1000),
            (1, 0.1, 10),
            (1, 0.0625, 16),
            (0.9, 0.3, 3),
            (0.025, 0.01, 3),
            (1 + 1e-12, 0.1, 11),
            (2.5e-15, 1e-15, 3),
            (0.001, 0.01, 1),
            (0, 0.1, 0),
        ]
        body = [2, 1, -2, 3, 4, -5, 6]
        for end, dt, expected_steps in cases:
            for threads in (None, 1, 4):
                with self.subTest(end=end, dt=dt, threads=threads):
                    _, rows = self.run_program([body], end=end, dt=dt, threads=threads)
                    self.assertEqual(len(rows), expected_steps + 1)
                    self.assertEqual(rows[-1][0], end)
                    self.assertTrue(all(a[0] < b[0] for a, b in zip(rows, rows[1:])))

                    # Одно тело движется равномерно: координаты известны без интегратора.
                    expected = [
                        [row[0]] + [body[axis + 1] + body[axis + 4] * row[0]
                                    for axis in range(3)]
                        for row in rows
                    ]
                    self.assert_rows(rows, expected)

    def test_zero_time_and_xy_format(self):
        # При нулевом времени в CSV должно остаться только начальное состояние.
        for threads in (None, 3):
            header, rows = self.run_program([[1, 1, 2, 3, 4, 5, 6]],
                                            end=0, threads=threads, fmt='xy')
            self.assertEqual(header, ['t', 'x1', 'y1'])
            self.assertEqual(rows, [[0, 1, 2]])

    def test_invalid_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            input_path = Path(directory) / 'input.txt'
            output_path = Path(directory) / 'output.csv'
            for executable in ('nbody_serial', 'nbody_pthreads'):
                # Ошибки формата, параметров расчёта и совпадающие координаты тел.
                cases = [('0\n', '1', '0.01'),
                         ('-1\n', '1', '0.01'),
                         ('1\n-1 0 0 0 0 0 0\n', '1', '0.01'),
                         ('1\n1 nan 0 0 0 0 0\n', '1', '0.01'),
                         ('1\n1 0 0\n', '1', '0.01'),
                         ('1\n1 0 0 0 0 0 0\nextra', '1', '0.01'),
                         ('1\n1 0 0 0 0 0 0\n', '-1', '0.01'),
                         ('1\n1 0 0 0 0 0 0\n', '1', '0'),
                         ('2\n1 0 0 0 0 0 0\n1 0 0 0 0 0 0\n', '1', '0.01')]
                for data, end, dt in cases:
                    input_path.write_text(data)
                    command = [str(ROOT / executable), end, str(input_path),
                               dt, str(output_path)]
                    result = subprocess.run(command, capture_output=True, timeout=15)
                    self.assertNotEqual(result.returncode, 0)
            input_path.write_text('1\n1 0 0 0 0 0 0\n')
            for threads in ('0', '-1', 'abc', '1025'):
                command = [str(ROOT / 'nbody_pthreads'), '1', str(input_path),
                           '0.01', str(output_path), threads]
                result = subprocess.run(command, capture_output=True, timeout=15)
                self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
