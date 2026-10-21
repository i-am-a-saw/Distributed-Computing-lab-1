#!/usr/bin/env python3
# Отдельная проверка двух готовых траекторий с допуском 1e-10 + 1e-10 * |a|.
import argparse
from pathlib import Path
from benchmark import compare


def main():
    parser = argparse.ArgumentParser(description='Сравнение траекторий из двух CSV-файлов.')
    parser.add_argument('serial', type=Path)
    parser.add_argument('parallel', type=Path)
    args = parser.parse_args()

    # Используем ту же проверку, что и при автоматических замерах.
    max_error, max_scaled_error, cells = compare(args.serial, args.parallel)
    print(
        f'PASS: {cells} values, '
        f'max_abs={max_error:.17g}, max_scaled={max_scaled_error:.17g}'
    )


if __name__ == '__main__':
    main()
