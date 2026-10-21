// Открывает объявления POSIX-барьеров в системных заголовках.
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "nbody.h"
#include "timer.h"

struct body_s* bodies;
// Каждый поток записывает силы только в свой массив.
struct vector_s** local_forces;
size_t body_count;
size_t thread_count;  // Число рабочих потоков; главный поток считается отдельно.
double current_step; // Главный поток задаёт шаг до стартового барьера.
int stop = 0;        // Сигнал завершения читается после стартового барьера.
pthread_barrier_t barrier;

// При сбое создания потоков ожидание старта предотвращает зависание на барьере.
pthread_mutex_t start_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;
int started = 0;  // 0: ожидание, 1: запуск, -1: ошибка создания.

struct thread_arg_s {
   size_t rank;
};

void Check_pthread(int error, const char* operation);
void Wait_barrier(void);
void* Thread_work(void* arg);

// Главный поток управляет шагами и выводом, рабочие потоки считают силы и движение.
int main(int argc, char* argv[]) {
   struct input_s input;
   pthread_t* thread_handles;
   struct thread_arg_s* args;
   FILE* output;
   size_t rank, created = 0, steps = 0;
   int error;
   double time = 0.0, next;
   double start, finish, total_start, total_finish, compute_time = 0.0;

   Get_options(argc, argv, 1, &input);
   thread_count = input.thread_count;
   bodies = Read_bodies(input.input_name, &body_count);
   thread_handles = Allocate(thread_count, sizeof(*thread_handles));
   // Аргументы остаются в памяти до завершения всех pthread_join.
   args = Allocate(thread_count, sizeof(*args));
   local_forces = Allocate(thread_count, sizeof(*local_forces));
   for (rank = 0; rank < thread_count; rank++)
      local_forces[rank] = Allocate(body_count, sizeof(**local_forces));
   // На каждой встрече ожидаются все рабочие потоки и главный поток.
   Check_pthread(pthread_barrier_init(&barrier, NULL, (unsigned)thread_count + 1),
                 "pthread_barrier_init");
   output = Open_output(&input, body_count);

   // Общее время включает создание потоков, запись CSV и ожидание завершения.
   GET_TIME(total_start);
   for (rank = 0; rank < thread_count; rank++) {
      args[rank].rank = rank;
      error = pthread_create(&thread_handles[rank], NULL, Thread_work, &args[rank]);
      if (error != 0) {
         fprintf(stderr, "pthread_create: %s\n", strerror(error));
         break;
      }
      created++;
   }
   Check_pthread(pthread_mutex_lock(&start_mutex), "pthread_mutex_lock");
   // При неполном создании группы уже запущенные потоки выйдут до барьера.
   started = created == thread_count ? 1 : -1;
   Check_pthread(pthread_cond_broadcast(&start_cond), "pthread_cond_broadcast");
   Check_pthread(pthread_mutex_unlock(&start_mutex), "pthread_mutex_unlock");

   if (created == thread_count) {
      Write_state(output, time, bodies, body_count, input.output_3d);
      while (time < input.end_time) {
         next = Next_time(time, steps, &input);
         current_step = next - time;
         // Этот интервал охватывает вычисления и ожидание трёх барьеров, без CSV.
         GET_TIME(start);
         Wait_barrier();  // 1: начало шага, current_step доступен всем потокам.
         Wait_barrier();  // 2: все силы вычислены по прежним координатам.
         Wait_barrier();  // 3: все координаты и скорости обновлены.
         GET_TIME(finish);
         compute_time += finish - start;
         time = next;
         steps++;
         Write_state(output, time, bodies, body_count, input.output_3d);
      }
      stop = 1;
      Wait_barrier();     // Освобождение потоков из ожидания следующего шага.
   }
   for (rank = 0; rank < created; rank++)
      Check_pthread(pthread_join(thread_handles[rank], NULL), "pthread_join");
   Close_output(output);
   GET_TIME(total_finish);
   if (created == thread_count) {
      printf("Bodies = %zu, threads = %zu, steps = %zu\n",
             body_count, thread_count, steps);
      printf("Compute time (with barriers) = %e seconds\n", compute_time);
      printf("Total time (with threads and CSV) = %e seconds\n",
             total_finish - total_start);
   }
   Check_pthread(pthread_barrier_destroy(&barrier), "pthread_barrier_destroy");
   Check_pthread(pthread_cond_destroy(&start_cond), "pthread_cond_destroy");
   Check_pthread(pthread_mutex_destroy(&start_mutex), "pthread_mutex_destroy");
   for (rank = 0; rank < thread_count; rank++) free(local_forces[rank]);
   free(local_forces);
   free(args);
   free(thread_handles);
   free(bodies);
   return created == thread_count ? EXIT_SUCCESS : EXIT_FAILURE;
}

// Pthreads возвращает код ошибки напрямую; для сообщения используется strerror.
void Check_pthread(int error, const char* operation) {
   if (error != 0) {
      fprintf(stderr, "%s: %s\n", operation, strerror(error));
      exit(EXIT_FAILURE);
   }
}

// PTHREAD_BARRIER_SERIAL_THREAD у одного участника означает успешную встречу.
void Wait_barrier(void) {
   int error = pthread_barrier_wait(&barrier);
   if (error != 0 && error != PTHREAD_BARRIER_SERIAL_THREAD)
      Check_pthread(error, "pthread_barrier_wait");
}

// Рабочий поток: вычислить свою часть пар, объединить силы и обновить часть тел.
void* Thread_work(void* arg) {
   size_t my_rank = ((struct thread_arg_s*)arg)->rank;
   struct vector_s* my_forces = local_forces[my_rank];
   struct vector_s zero = {0.0, 0.0, 0.0};
   size_t i, j, rank;
   int run;

   Check_pthread(pthread_mutex_lock(&start_mutex), "pthread_mutex_lock");
   // Условие проверяется в while, поскольку возможны ложные пробуждения.
   while (started == 0)
      Check_pthread(pthread_cond_wait(&start_cond, &start_mutex), "pthread_cond_wait");
   run = started;
   Check_pthread(pthread_mutex_unlock(&start_mutex), "pthread_mutex_unlock");
   if (run < 0) return NULL;

   while (1) {
      Wait_barrier();  // 1: главный поток задал шаг и сигнал завершения.
      if (stop) break;
      for (i = 0; i < body_count; i++) my_forces[i] = zero;

      // Строки верхнего треугольника распределяются циклически: каждая пара i < j ровно один раз.
      for (i = my_rank; i < body_count; i += thread_count) {
         for (j = i + 1; j < body_count; j++) {
            struct vector_s force = Pair_force(&bodies[i], &bodies[j]);
            Add_force(&my_forces[i], force);
            Subtract_force(&my_forces[j], force);
         }
      }
      Wait_barrier();  // 2: чтение прежних координат завершено во всех потоках.

      // Диапазоны тел не пересекаются: у каждого тела ровно один поток-писатель.
      for (i = body_count*my_rank/thread_count;
           i < body_count*(my_rank + 1)/thread_count; i++) {
         struct vector_s total = {0.0, 0.0, 0.0};
         // Для своего тела нужно собрать вклады всех потоков, включая собственный.
         for (rank = 0; rank < thread_count; rank++)
            Add_force(&total, local_forces[rank][i]);
         Update_body(&bodies[i], total, current_step);
      }
      Wait_barrier();  // 3: главный поток может записывать новое состояние в CSV.
   }
   return NULL;
}
