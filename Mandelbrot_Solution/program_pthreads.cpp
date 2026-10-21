// Подключаем функции преобразования аргументов командной строки в числа
#include <cstdlib>
// Подключаем функцию strerror для текста ошибок pthreads
#include <cstring>
// Подключаем классы для записи данных в файл
#include <fstream>
// Подключаем средства настройки точности и формата вывода чисел
#include <iomanip>
// Подключаем вывод сообщений в консоль
#include <iostream>
// Подключаем информацию о максимально допустимых значениях типов
#include <limits>
// Подключаем генератор случайных чисел и распределения
#include <random>
// Подключаем динамические массивы
#include <vector>
// Подключаем POSIX-потоки (pthreads)
#include <pthread.h>
// Подключаем файл timer.h, в котором определён макрос измерения времени GET_TIME
#include "timer.h"

// Объявляем тип Point для хранения координат точки x и y
struct Point { double x, y; };

// Проверяет точку (cx, cy); max_iter задаёт максимальное число итераций (по умолчанию 1000)
bool is_mandelbrot(double cx, double cy, int max_iter = 1000) {
    // Начинаем вычисление последовательности с z = 0
    double x = 0.0, y = 0.0;
    for (int i = 0; i < max_iter; ++i) {
        const double xx = x * x;
        const double yy = y * y;
        // Если квадрат модуля не меньше 4 (модуль достиг 2), точка отвергается
        if (xx + yy >= 4.0) return false;
        // z = z^2 + c сначала новая мнимая часть, затем действительная
        const double next_y = 2.0 * x * y + cy;
        x = xx - yy + cx;
        y = next_y;
    }
    // За max_iter итераций модуль не достиг 2 - приближённо считаем точку принадлежащей множеству
    return true;
}

// Общие данные, разделяемые всеми потоками
struct SharedState {
    size_t npoints;             // сколько точек нужно получить всего
    size_t accepted;            // сколько точек уже принято (защищено мьютексом)
    pthread_mutex_t mutex;      // мьютекс, защищающий счётчик accepted
};

// Аргументы, передаваемые в каждый поток через pthread_create
struct ThreadArgs {
    int thread_id;                  // номер потока
    SharedState* shared;            // указатель на общие данные
    std::vector<Point>* output;     // локальный список точек этого потока
};

// Функция потока, pthreads требует сигнатуру void* (*)(void*)
void* worker(void* arg) {
    // Приводим void* обратно к указателю на аргументы потока
    ThreadArgs* args = static_cast<ThreadArgs*>(arg);
    SharedState* shared = args->shared;
    std::vector<Point>& output = *args->output;

    // У каждого потока свой генератор со своим начальным значением
    std::mt19937_64 rng(0x9E3779B97F4A7C15ULL + static_cast<unsigned long long>(args->thread_id));
    // Равномерная генерация x в [-2, 1] и y в [-1.5, 1.5]
    std::uniform_real_distribution<double> dist_x(-2.0, 1.0);
    std::uniform_real_distribution<double> dist_y(-1.5, 1.5);

    while (true) {
        // Генерируем случайную точку
        const double x = dist_x(rng);
        const double y = dist_y(rng);
        // Дорогая проверка выполняется без блокировки, параллельно в каждом потоке
        if (is_mandelbrot(x, y)) {
            bool store = false;
            bool done = false;
            // Захватываем мьютекс, чтобы безопасно прочитать и изменить общий счётчик
            pthread_mutex_lock(&shared->mutex);
            if (shared->accepted < shared->npoints) {
                // Ещё есть свободное место, резервируем его за этой точкой
                ++shared->accepted;
                store = true;
                // Если эта точка была последней нужной, поток может завершаться
                if (shared->accepted == shared->npoints) done = true;
            } else {
                // Нужное количество уже набрано другими потоками
                done = true;
            }
            // Освобождаем мьютекс как можно раньше
            pthread_mutex_unlock(&shared->mutex);

            // Записываем точку в свой локальный список (без блокировки он только наш)
            if (store) output.push_back({x, y});
            if (done) break;
        }
    }
    // Поток ничего не возвращает через pthread_join
    return nullptr;
}

// Главная функция программы, argc - количество аргументов, argv - сами аргументы
int main(int argc, char** argv) {
    // Нужны ровно два аргумента - число потоков и число точек
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " nthreads npoints\n";
        return 1;
    }
    char* end = nullptr;
    // Разбираем число потоков и проверяем диапазон [1, 1024]
    long thread_arg = std::strtol(argv[1], &end, 10);
    if (*end != '\0' || thread_arg < 1 || thread_arg > 1024) {
        std::cerr << "nthreads must be an integer in [1, 1024]\n";
        return 1;
    }
    end = nullptr;
    // Разбираем число точек: положительное целое, помещающееся в size_t
    unsigned long long point_arg = std::strtoull(argv[2], &end, 10);
    if (*end != '\0' || argv[2][0] == '-' || point_arg == 0 ||
        point_arg > static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
        std::cerr << "npoints must be a positive integer\n";
        return 1;
    }

    const size_t npoints = static_cast<size_t>(point_arg);
    const int nthreads = static_cast<int>(thread_arg);

    // Инициализируем общие данные и мьютекс
    SharedState shared;
    shared.npoints = npoints;
    shared.accepted = 0;
    pthread_mutex_init(&shared.mutex, nullptr);

    // Отдельный список точек для каждого потока - потоки не пишут в общий список одновременно
    std::vector<std::vector<Point>> local_points(static_cast<size_t>(nthreads));
    // Идентификаторы потоков pthread_t
    std::vector<pthread_t> workers(static_cast<size_t>(nthreads));
    // Аргументы для каждого потока; живут до pthread_join, поэтому указатели на них корректны
    std::vector<ThreadArgs> args(static_cast<size_t>(nthreads));

    double start_time;
    GET_TIME(start_time);

    // Создаём ровно nthreads рабочих потоков
    int created = 0;
    for (int t = 0; t < nthreads; ++t) {
        args[static_cast<size_t>(t)] = ThreadArgs{t, &shared, &local_points[static_cast<size_t>(t)]};
        int rc = pthread_create(&workers[static_cast<size_t>(t)], nullptr, worker,
                                &args[static_cast<size_t>(t)]);
        if (rc != 0) {
            std::cerr << "pthread_create failed: " << std::strerror(rc) << "\n";
            // Дожидаемся уже созданных потоков, они завершатся сами, набрав npoints точек
            for (int k = 0; k < created; ++k) pthread_join(workers[static_cast<size_t>(k)], nullptr);
            pthread_mutex_destroy(&shared.mutex);
            return 2;
        }
        ++created;
    }
    // Ожидаем завершения всех потоков перед сбором результатов
    for (int t = 0; t < nthreads; ++t) pthread_join(workers[static_cast<size_t>(t)], nullptr);

    // Мьютекс больше не нужен
    pthread_mutex_destroy(&shared.mutex);

    // Объединяем локальные списки потоков в один общий
    std::vector<Point> points;
    points.reserve(npoints);
    for (const auto& part : local_points)
        for (const auto& p : part)
            if (points.size() < npoints) points.push_back(p);

    // Счётчик увеличивается только под мьютексом и не выше npoints,
    // поэтому всего должно быть сохранено ровно npoints точек
    if (points.size() != npoints) {
        std::cerr << "Internal error: generated " << points.size() << " points, expected " << npoints << "\n";
        return 2;
    }

    // Записываем точки в CSV
    std::ofstream csv("mandelbrot.csv");
    if (!csv) {
        std::cerr << "Cannot open mandelbrot.csv for writing\n";
        return 2;
    }
    csv << "x,y\n" << std::setprecision(17);
    for (const auto& p : points) csv << p.x << ',' << p.y << '\n';
    csv.close();
    if (!csv) {
        std::cerr << "Error while writing mandelbrot.csv\n";
        return 2;
    }

    double end_time;
    GET_TIME(end_time);
    std::cout << "Generated " << npoints << " points in mandelbrot.csv using "
              << nthreads << " thread(s). Elapsed: " << std::fixed << std::setprecision(6)
              << (end_time - start_time) << " s\n";
    return 0;
}
