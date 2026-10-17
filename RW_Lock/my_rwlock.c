#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_KEY 100000000U

typedef struct list_node_s {
	int data;
	struct list_node_s *next;
} list_node_t;

static list_node_t *head = NULL;

typedef struct {
	pthread_mutex_t mutex;
	pthread_cond_t readers_cv;
	pthread_cond_t writers_cv;
	unsigned active_readers;
	unsigned waiting_readers;
	unsigned waiting_writers;
	int writer_active;
} my_rwlock_t;

static int my_rwlock_init(my_rwlock_t *lock) {
	int rc;

	if (lock == NULL) return EINVAL;
	memset(lock, 0, sizeof(*lock)); // обнулить байты лока

	rc = pthread_mutex_init(&lock->mutex, NULL);
	if (rc != 0) return rc;

	rc = pthread_cond_init(&lock->readers_cv, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&lock->mutex);
		return rc;
	}

	rc = pthread_cond_init(&lock->writers_cv, NULL);
	if (rc != 0) {
		pthread_cond_destroy(&lock->readers_cv);
		pthread_mutex_destroy(&lock->mutex);
		return rc;
	}

	return 0;
}

static int my_rwlock_destroy(my_rwlock_t *lock) {
	int rc1, rc2, rc3;

	if (lock == NULL) return EINVAL;
	rc1 = pthread_cond_destroy(&lock->readers_cv);
	rc2 = pthread_cond_destroy(&lock->writers_cv);
	rc3 = pthread_mutex_destroy(&lock->mutex);
	if (rc1 != 0) return rc1;
	if (rc2 != 0) return rc2;
	
	return rc3;
}

static int my_rwlock_rdlock(my_rwlock_t *lock) {
	int rc;
	
	rc = pthread_mutex_lock(&lock->mutex);
	if (rc != 0) return rc;

	lock->waiting_readers++;
	while (lock->writer_active || lock->waiting_writers > 0) {
		rc = pthread_cond_wait(&lock->readers_cv, &lock->mutex); // поток отпустит mutex, чтобы другие могли завершиться (например писатели отпустить блокировку)
		if (rc != 0) {
			lock->waiting_readers--;
			pthread_mutex_unlock(&lock->mutex);
			return rc;
		}
	}

	lock->waiting_readers--;
	lock->active_readers++;

	return pthread_mutex_unlock(&lock->mutex);
}

static int my_rwlock_wrlock(my_rwlock_t *lock) {
	int rc;

	rc = pthread_mutex_lock(&lock->mutex);
	if (rc != 0) return rc;

	lock->waiting_writers++;

	while (lock->writer_active || lock->active_readers > 0) {
		rc = pthread_cond_wait(&lock->writers_cv, &lock->mutex);
		// после пробуждения, поток еще раз проверит условия while
		

		if (rc != 0) {
			lock->waiting_writers--;
			pthread_mutex_unlock(&lock->mutex);
			return rc;
		}
	}

	lock->waiting_writers--;
	lock->writer_active = 1;

	return pthread_mutex_unlock(&lock->mutex);
}

static int my_rwlock_unlock(my_rwlock_t *lock) {
	int rc;

	rc = pthread_mutex_lock(&lock->mutex);
	if (rc != 0) return rc;

	if (lock->writer_active) {
		lock->writer_active = 0;
	} else if (lock->active_readers > 0) {
		lock->active_readers--;
	} else {
		pthread_mutex_unlock(&lock->mutex);
		return EPERM;
	}

	if (!lock->writer_active && lock->active_readers == 0 &&
		lock->waiting_writers > 0) {
		rc = pthread_cond_signal(&lock->writers_cv); // signal пробудит только одного писателя
	} else if (!lock->writer_active && lock->waiting_writers == 0) {
		rc = pthread_cond_broadcast(&lock->readers_cv); // разбудит всех ожидающих читателей
	} else {
		rc = 0;
	}

	{
		int unlock_rc = pthread_mutex_unlock(&lock->mutex);
		return rc != 0 ? rc : unlock_rc;
	}
}

typedef enum {
	LOCK_PTHREAD,
	LOCK_CUSTOM
} lock_kind_t;

static pthread_rwlock_t system_lock;
static my_rwlock_t custom_lock;
static lock_kind_t selected_lock;

static int read_lock(void) {
	return selected_lock == LOCK_CUSTOM
		? my_rwlock_rdlock(&custom_lock)
		: pthread_rwlock_rdlock(&system_lock);
}

static int write_lock(void) {
	return selected_lock == LOCK_CUSTOM
		? my_rwlock_wrlock(&custom_lock)
		: pthread_rwlock_wrlock(&system_lock);
}

static int rw_unlock(void) {
	return selected_lock == LOCK_CUSTOM
		? my_rwlock_unlock(&custom_lock)
		: pthread_rwlock_unlock(&system_lock);
}

static int lock_init(lock_kind_t kind) {
	selected_lock = kind;
	return kind == LOCK_CUSTOM
		? my_rwlock_init(&custom_lock)
		: pthread_rwlock_init(&system_lock, NULL);
}

static int lock_destroy(lock_kind_t kind) {
	return kind == LOCK_CUSTOM
		? my_rwlock_destroy(&custom_lock)
		: pthread_rwlock_destroy(&system_lock);
}

static uint32_t rng_next(uint32_t *state) {
    uint32_t x = *state;
    if (x == 0) x = 0xA341316CU;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static double rng_double(uint32_t *state) {
    return (double)rng_next(state) / 4294967296.0;
}



// -1 => ошибка памяти
// 0 => неуспешное выполнение
// 1 => успешное выполнение



static int list_insert(int value) {
    list_node_t *curr = head;
    list_node_t *pred = NULL;
    list_node_t *node;

    while (curr != NULL && curr->data < value) {
        pred = curr;
        curr = curr->next;
    }

    if (curr != NULL && curr->data == value) return 0;

    node = malloc(sizeof(*node));
    if (node == NULL) return -1;
    node->data = value;
    node->next = curr;
    if (pred == NULL) head = node;
    else pred->next = node;
    return 1;
}

static int list_member(int value) {
    list_node_t *curr = head;
    while (curr != NULL && curr->data < value) curr = curr->next;
    return curr != NULL && curr->data == value;
}

static int list_delete(int value) {
    list_node_t *curr = head;
    list_node_t *pred = NULL;

    while (curr != NULL && curr->data < value) {
        pred = curr;
        curr = curr->next;
    }
    if (curr == NULL || curr->data != value) return 0;

    if (pred == NULL) head = curr->next;
    else pred->next = curr->next;
    free(curr);
    return 1;
}

static void list_free(void) {
    list_node_t *curr = head;
    while (curr != NULL) {
        list_node_t *next = curr->next;
        free(curr);
        curr = next;
    }
    head = NULL;
}

static uint64_t list_size_and_validate(int *valid) {
    list_node_t *curr = head;
    uint64_t count = 0;
    int previous = -1;
    *valid = 1;

    while (curr != NULL) {
        if (curr->data < 0 || (count > 0 && curr->data <= previous)) {
            *valid = 0;
            break;
        }
        previous = curr->data;
        count++;
        curr = curr->next;
    }
    return count;
}

static int populate_initial_list(uint64_t wanted, uint64_t *inserted) {
	uint32_t seed = 1;
	uint64_t attempts = 0;
	uint64_t max_attempts = wanted * 2;
	*inserted = 0;

	while (*inserted < wanted && attempts < max_attempts) {
		int key = (int)(rng_next(&seed) % MAX_KEY);
		int rc = list_insert(key);
		if (rc < 0) return -1;
		if (rc == 1) (*inserted)++;
		attempts++;
	}

	return 0;
}

typedef struct {
	uint64_t member_ops;  // кол-во операций поиска
        uint64_t insert_ops;
        uint64_t delete_ops;
        uint64_t member_hits;  // кол-во успешных поисков
        uint64_t inserted;
        uint64_t deleted;
        uint64_t allocation_errors;
} thread_result_t;

typedef struct {
	long rank;
	uint64_t ops;
	double search_fraction;
	double insert_fraction;
	thread_result_t result;
} thread_arg_t;



static void *thread_work(void *arg_ptr) {
	thread_arg_t *arg = (thread_arg_t *)arg_ptr;
	uint32_t seed = (uint32_t)arg->rank + 1U;
	uint64_t i;

	memset(&arg->result, 0, sizeof(arg->result));
	for (i = 0; i < arg->ops; i++) {
		double which_op = rng_double(&seed);
		int value = (int)(rng_next(&seed) % MAX_KEY);
		int rc;

		if (which_op < arg->search_fraction) {
			rc = read_lock();
			if (rc != 0) {
				arg->result.allocation_errors++;
				continue;
			}
			arg->result.member_hits += (uint64_t)list_member(value);
			rc = rw_unlock();
			if (rc != 0) arg->result.allocation_errors++;
			arg->result.member_ops++;
		} else if (which_op < arg->search_fraction + arg->insert_fraction) {
			rc = write_lock();
			if (rc != 0) {
				arg->result.allocation_errors++;
				continue;
			}
			rc = list_insert(value);
			if (rc == 1) arg->result.inserted++;
			else if (rc < 0) arg->result.allocation_errors++;
			if (rw_unlock() != 0) arg->result.allocation_errors++;
			arg->result.insert_ops++;
		} else {
			rc = write_lock();
			if (rc != 0) {
				arg->result.allocation_errors++;
				continue;
			}
			arg->result.deleted += (uint64_t)list_delete(value);
			if (rw_unlock() != 0) arg->result.allocation_errors++;
			arg->result.delete_ops++;
		}
	}

	return NULL;
}



typedef struct {
	double elapsed_seconds;
	uint64_t member_ops, insert_ops, delete_ops;
	uint64_t member_hits, inserted, deleted, errors;
	uint64_t initial_inserted, final_size;
	int list_valid;
} bench_result_t;

static double monotonic_seconds(void) {
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		perror("clock_gettime");
		exit(EXIT_FAILURE);
	}
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1.0e9; // количество секунд с дробью
}

static int run_benchmark(lock_kind_t kind, int thread_count, uint64_t total_ops, 
		uint64_t initial_keys, double search_fraction,
		double insert_fraction, bench_result_t *out) {
	
	pthread_t *threads = NULL;
	thread_arg_t *args = NULL;
	uint64_t i, base_ops, extra_ops, initial_inserted = 0;
	size_t created = 0;
	double start, finish;
	int rc = 0;

	memset(out, 0, sizeof(*out));
	head = NULL;
	if (populate_initial_list(initial_keys, &initial_inserted) != 0) {
		fprintf(stderr, "Ошибка выделения памяти в начале\n");
		list_free();
		return -1;
	}
	out->initial_inserted = initial_inserted;

	threads = malloc((size_t)thread_count * sizeof *threads);
	args = malloc((size_t)thread_count * sizeof *args);
	if (threads == NULL || args == NULL) {
		fprintf(stderr, "Память под потоки не выделена :(\n");
		free(threads);
		free(args);
		list_free();
		return -1;
	}

	rc = lock_init(kind);
	if (rc != 0) {
		fprintf(stderr, "Не удалось инициализировать блокировку :( %s\n", strerror(rc));
        	free(threads);
        	free(args);
                list_free();
        	return -1;
	}

	base_ops = total_ops / (uint64_t)thread_count;
	extra_ops = total_ops % (uint64_t)thread_count;
	for (i = 0; i < (uint64_t)thread_count; i++) {
		args[i].rank = (long)i;
		args[i].ops = base_ops + (i < extra_ops ? 1U : 0U); // все лишние раскидаем равномерно по потокам
		args[i].search_fraction = search_fraction;
		args[i].insert_fraction = insert_fraction;
	}


	// ИЗМЕРЕНИЕ
	start = monotonic_seconds();
	for (i = 0; i < (uint64_t)thread_count; i++) {
		rc = pthread_create(&threads[i], NULL, thread_work, &args[i]);
		if (rc != 0) {
			fprintf(stderr, "Поток %ld не создан: %s\n", (long)i, strerror(rc));
			break;
		}
		++created;
	}
	for (i = 0; i < (uint64_t)thread_count; i++) {
		rc = pthread_join(threads[i], NULL);
		if (rc != 0) {
			fprintf(stderr, "Join не удался: %s\n", strerror(rc));
			rc = -1;
		}
	}
	finish = monotonic_seconds();
	// КОНЕЦ ИЗМЕРЕНИЯ
	//
	
	out->elapsed_seconds = finish - start;

	for (i = 0; i < (uint64_t)created; i++) {
		out->member_ops += args[i].result.member_ops;
        	out->insert_ops += args[i].result.insert_ops;
        	out->delete_ops += args[i].result.delete_ops;
        	out->member_hits += args[i].result.member_hits;
        	out->inserted += args[i].result.inserted;
        	out->deleted += args[i].result.deleted;
        	out->errors += args[i].result.allocation_errors;
	}


	out->final_size = list_size_and_validate(&out->list_valid);


	{
		int drc = lock_destroy(kind);
		if (drc != 0) {
			fprintf(stderr, "Не уничтожили блокировку: %s\n", strerror(drc));
			rc = -1;
		}
	}


	free(threads);
	free(args);
	list_free();

	if (created != (size_t)thread_count || rc != 0) return -1;
    	return 0;

}

static void print_result(const char *label, const bench_result_t *r,
		uint64_t requested_ops) {
	printf("%s: %.6f с, %.0f операций/с\n", label, r->elapsed_seconds,
           r->elapsed_seconds > 0.0 ? (double)requested_ops / r->elapsed_seconds : 0.0);
    	printf("  фактически операций: member=%" PRIu64 ", insert=%" PRIu64
           ", delete=%" PRIu64 " (всего=%" PRIu64 ")\n",
           r->member_ops, r->insert_ops, r->delete_ops,
           r->member_ops + r->insert_ops + r->delete_ops);
    	printf("  найдено=%" PRIu64 ", успешно вставлено=%" PRIu64
           ", удалено=%" PRIu64 ", ошибок=%" PRIu64 "\n",
           r->member_hits, r->inserted, r->deleted, r->errors);
    	printf("  начальный список: %" PRIu64 " узлов, итоговый: %" PRIu64
           " узлов, порядок/уникальность: %s\n",
           r->initial_inserted, r->final_size, r->list_valid ? "OK" : "ОШИБКА");
}

static int parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long v;
    if (text == NULL || *text == '\0' || *text == '-') return -1;
    errno = 0;
    v = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return -1;
    *value = (uint64_t)v;
    return 0;
}

static int parse_double_arg(const char *text, double *value)
{
    char *end = NULL;
    double v;
    errno = 0;
    v = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0') return -1;
    *value = v;
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr,
        "Использование: %s <потоки> <всего_операций> <начальные_ключи> "
        "<доля_поиска> <доля_вставки> [повторы]\n"
        "Пример: %s 4 1000000 10000 0.80 0.10 3\n"
        "Доля удаления вычисляется как 1 - доля_поиска - доля_вставки.\n",
        program, program);
}

int main(int argc, char **argv) {
	uint64_t total_ops, initial_keys, repetitions = 1, rep;
	uint64_t threads_u64;
	double search_fraction, insert_fraction, delete_fraction;
	double system_total = 0.0, custom_total = 0.0;
	int thread_count;
	bench_result_t system_result, custom_result;

	if (argc != 6 && argc != 7) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	if (parse_u64(argv[1], &threads_u64) != 0 || threads_u64 == 0 ||
        	threads_u64 > 4096 ||
        	parse_u64(argv[2], &total_ops) != 0 || total_ops == 0 ||
        	parse_u64(argv[3], &initial_keys) != 0 ||
        	initial_keys > UINT64_MAX / 2 ||
        	parse_double_arg(argv[4], &search_fraction) != 0 ||
        	parse_double_arg(argv[5], &insert_fraction) != 0 ||
        	(argc == 7 && (parse_u64(argv[6], &repetitions) != 0 || repetitions == 0)) ||
        	search_fraction < 0.0 || search_fraction > 1.0 ||
        	insert_fraction < 0.0 || insert_fraction > 1.0 ||
        	search_fraction + insert_fraction > 1.0 + 1e-12) {
        		usage(argv[0]);
        		fprintf(stderr, "Некорректные параметры. Проверьте числа и доли операций.\n");
        		return EXIT_FAILURE;
    	}

	thread_count = (int)threads_u64;
	delete_fraction = 1.0 - search_fraction - insert_fraction;

	printf("Потоков: %d; операций: %" PRIu64 "; начальных ключей: %" PRIu64 "\n",
           thread_count, total_ops, initial_keys);
    	printf("Распределение: поиск=%.2f%%, вставка=%.2f%%, удаление=%.2f%%; повторов=%" PRIu64 "\n\n",
           search_fraction * 100.0, insert_fraction * 100.0,
           delete_fraction * 100.0, repetitions);

	for (rep = 0; rep < repetitions; rep++) {
		if ((rep % 2) == 0) {
            		if (run_benchmark(LOCK_PTHREAD, thread_count, total_ops, initial_keys,
                              search_fraction, insert_fraction, &system_result) != 0 ||
                	run_benchmark(LOCK_CUSTOM, thread_count, total_ops, initial_keys,
                              search_fraction, insert_fraction, &custom_result) != 0) {
                	return EXIT_FAILURE;
            		}
        	} else {
            		if (run_benchmark(LOCK_CUSTOM, thread_count, total_ops, initial_keys,
                              search_fraction, insert_fraction, &custom_result) != 0 ||
                run_benchmark(LOCK_PTHREAD, thread_count, total_ops, initial_keys,
                              search_fraction, insert_fraction, &system_result) != 0) {
                		return EXIT_FAILURE;
            		}
        	}

		system_total += system_result.elapsed_seconds;
		custom_total += custom_result.elapsed_seconds;
		
		printf("=== Повтор %" PRIu64 " из %" PRIu64 " ===\n", rep + 1, repetitions);
        	print_result("Библиотечный pthread_rwlock_t", &system_result, total_ops);
        	print_result("Собственный my_rwlock_t", &custom_result, total_ops);
        	if (system_result.errors != 0 || custom_result.errors != 0 ||
            		!system_result.list_valid || !custom_result.list_valid ||
            		system_result.member_ops + system_result.insert_ops + system_result.delete_ops != total_ops ||
            		custom_result.member_ops + custom_result.insert_ops + custom_result.delete_ops != total_ops) {
            		fprintf(stderr, "Проверка результата не пройдена.\n");
            		return EXIT_FAILURE;
        	}

        	putchar('\n');

	}

	{
        	double system_avg = system_total / (double)repetitions;
        	double custom_avg = custom_total / (double)repetitions;
        	double delta_pct = system_avg > 0.0
            		? (custom_avg - system_avg) / system_avg * 100.0 : 0.0;
        	printf("=== Среднее по %" PRIu64 " повторам ===\n", repetitions);
        	printf("pthread_rwlock_t: %.6f с, %.0f операций/с\n",
               		system_avg, (double)total_ops / system_avg);
        	printf("my_rwlock_t:      %.6f с, %.0f операций/с\n",
               		custom_avg, (double)total_ops / custom_avg);
        	printf("Собственная реализация %s на %.2f%% по времени.\n",
               		delta_pct > 0.0 ? "медленнее" : (delta_pct < 0.0 ? "быстрее" : "равна"),
               		delta_pct >= 0.0 ? delta_pct : -delta_pct);
    	}	
    	
	return EXIT_SUCCESS;
}
