#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <string.h>
#include "nbody.h"

const double G = 6.67430e-11;  // Гравитационная постоянная в единицах СИ.

static void Usage(const char* program, int parallel);
static double Read_number(const char* text);

// Выделение обнулённого массива с проверкой размера и наличия памяти.
void* Allocate(size_t count, size_t size) {
   void* memory;
   if (size != 0 && count > SIZE_MAX/size) {
      fprintf(stderr, "Requested array is too large\n");
      exit(EXIT_FAILURE);
   }
   memory = calloc(count, size);
   if (memory == NULL) {
      fprintf(stderr, "Not enough memory\n");
      exit(EXIT_FAILURE);
   }
   return memory;
}

// Некорректные аргументы: вывести формат запуска и завершить программу с ошибкой.
static void Usage(const char* program, int parallel) {
   fprintf(stderr, "Usage: %s tend input.txt [dt [output.csv%s]]\n",
           program, parallel ? " [threads [xy|xyz]]" : " [xy|xyz]");
   fprintf(stderr, "Defaults: dt=0.01, output=trajectory.csv%s, format=xy\n",
           parallel ? ", threads=2" : "");
   exit(EXIT_FAILURE);
}

// Преобразование строки целиком: частичная запись числа, NaN и бесконечность запрещены.
static double Read_number(const char* text) {
   char* end;
   double value;
   errno = 0;
   value = strtod(text, &end);
   if (text == end || *end != '\0' || errno != 0 || !isfinite(value)) {
      fprintf(stderr, "Invalid number: %s\n", text);
      exit(EXIT_FAILURE);
   }
   return value;
}

// Общий разбор параметров; parallel выбирает наличие аргумента с числом потоков.
void Get_options(int argc, char* argv[], int parallel, struct input_s* input) {
   char* end;
   unsigned long value;
   int format_index = parallel ? 6 : 5;

   if (argc < 3 || argc > (parallel ? 7 : 6)) Usage(argv[0], parallel);
   input->end_time = Read_number(argv[1]);
   input->input_name = argv[2];
   input->time_step = argc > 3 ? Read_number(argv[3]) : 0.01;
   input->output_name = argc > 4 ? argv[4] : "trajectory.csv";
   input->thread_count = parallel ? 2 : 1;
   input->output_3d = 0;
   if (input->end_time < 0.0 || input->time_step <= 0.0) {
      fprintf(stderr, "Require tend >= 0 and dt > 0\n");
      exit(EXIT_FAILURE);
   }
   if (parallel && argc > 5) {
      errno = 0;
      value = strtoul(argv[5], &end, 10);
      if (argv[5][0] == '-' || argv[5] == end || *end != '\0' ||
          errno != 0 || value < 1 || value > 1024) {
         fprintf(stderr, "Thread count must be in [1, 1024]\n");
         exit(EXIT_FAILURE);
      }
      input->thread_count = (size_t)value;
   }
   if (argc > format_index) {
      if (strcmp(argv[format_index], "xyz") == 0) input->output_3d = 1;
      else if (strcmp(argv[format_index], "xy") != 0) Usage(argv[0], parallel);
   }
}

// Чтение начальных условий с проверкой масс, координат и скоростей.
struct body_s* Read_bodies(const char* file_name, size_t* body_count) {
   FILE* file = fopen(file_name, "r");
   struct body_s* bodies;
   char count_text[64];
   char* end;
   unsigned long long count;
   size_t i;
   int extra;

   if (file == NULL) { perror(file_name); exit(EXIT_FAILURE); }
   if (fscanf(file, "%63s", count_text) != 1) {
      fprintf(stderr, "Missing body count\n");
      exit(EXIT_FAILURE);
   }
   errno = 0;
   count = strtoull(count_text, &end, 10);
   if (count_text[0] == '-' || count_text == end || *end != '\0' ||
       errno != 0 || count == 0 || count > SIZE_MAX/sizeof(*bodies)) {
      fprintf(stderr, "Invalid body count\n");
      exit(EXIT_FAILURE);
   }
   *body_count = (size_t)count;
   bodies = Allocate(*body_count, sizeof(*bodies));
   for (i = 0; i < *body_count; i++) {
      struct body_s* b = &bodies[i];
      if (fscanf(file, "%lf %lf %lf %lf %lf %lf %lf", &b->mass,
                 &b->position.x, &b->position.y, &b->position.z,
                 &b->velocity.x, &b->velocity.y, &b->velocity.z) != 7 ||
          !isfinite(b->mass) || b->mass <= 0.0 ||
          !isfinite(b->position.x) || !isfinite(b->position.y) ||
          !isfinite(b->position.z) || !isfinite(b->velocity.x) ||
          !isfinite(b->velocity.y) || !isfinite(b->velocity.z)) {
         fprintf(stderr, "Invalid data for body %zu\n", i + 1);
         exit(EXIT_FAILURE);
      }
   }
   // Дополнительные данные после заданного числа тел считаются ошибкой ввода.
   extra = fgetc(file);
   while (extra == ' ' || extra == '\n' || extra == '\r' || extra == '\t')
      extra = fgetc(file);
   if (extra != EOF || ferror(file)) {
      fprintf(stderr, "Unexpected data or read error after last body\n");
      exit(EXIT_FAILURE);
   }
   if (fclose(file) != 0) { perror("close input"); exit(EXIT_FAILURE); }
   return bodies;
}

// Открытие CSV с перезаписью и заголовком для проекции xy или полного вывода xyz.
FILE* Open_output(const struct input_s* input, size_t body_count) {
   size_t i;
   FILE* file = fopen(input->output_name, "w");
   if (file == NULL) { perror(input->output_name); exit(EXIT_FAILURE); }
   fprintf(file, "t");
   for (i = 0; i < body_count; i++) {
      fprintf(file, ",x%zu,y%zu", i + 1, i + 1);
      if (input->output_3d) fprintf(file, ",z%zu", i + 1);
   }
   fprintf(file, "\n");
   return file;
}

// Одна строка CSV содержит время и координаты всех тел; %.17g сохраняет точность записи.
void Write_state(FILE* file, double time, const struct body_s bodies[],
                 size_t body_count, int output_3d) {
   size_t i;
   fprintf(file, "%.17g", time);
   for (i = 0; i < body_count; i++) {
      fprintf(file, ",%.17g,%.17g", bodies[i].position.x, bodies[i].position.y);
      if (output_3d) fprintf(file, ",%.17g", bodies[i].position.z);
   }
   fprintf(file, "\n");
   if (ferror(file)) { perror("write CSV"); exit(EXIT_FAILURE); }
}

// Ошибка отложенной записи может обнаружиться только при закрытии файла.
void Close_output(FILE* file) {
   if (fclose(file) != 0) { perror("close CSV"); exit(EXIT_FAILURE); }
}

// Сила притяжения, действующая на первое тело со стороны второго.
struct vector_s Pair_force(const struct body_s* first,
                           const struct body_s* second) {
   struct vector_s force;
   // Вектор от первого тела ко второму задаёт направление силы притяжения.
   double dx = second->position.x - first->position.x;
   double dy = second->position.y - first->position.y;
   double dz = second->position.z - first->position.z;
   double distance = hypot(hypot(dx, dy), dz);
   double coefficient;

   // При совпадении точечных масс сила в этой модели не определена.
   if (distance == 0.0 || !isfinite(distance)) {
      fprintf(stderr, "Zero or non-finite distance between bodies\n");
      exit(EXIT_FAILURE);
   }
   coefficient = (G*first->mass/distance)*(second->mass/distance)/distance;
   force.x = coefficient*dx;
   force.y = coefficient*dy;
   force.z = coefficient*dz;
   if (!isfinite(force.x) || !isfinite(force.y) || !isfinite(force.z)) {
      fprintf(stderr, "Non-finite force; check masses and distances\n");
      exit(EXIT_FAILURE);
   }
   return force;
}

void Add_force(struct vector_s* sum, struct vector_s force) {
   sum->x += force.x;
   sum->y += force.y;
   sum->z += force.z;
}

void Subtract_force(struct vector_s* sum, struct vector_s force) {
   sum->x -= force.x;
   sum->y -= force.y;
   sum->z -= force.z;
}

// Шаг Эйлера для одного тела; сила должна быть рассчитана по прежним координатам.
void Update_body(struct body_s* body, struct vector_s force, double step) {
   // В явном методе Эйлера координаты обновляются по прежней скорости.
   body->position.x += body->velocity.x*step;
   body->position.y += body->velocity.y*step;
   body->position.z += body->velocity.z*step;
   body->velocity.x += (force.x/body->mass)*step;
   body->velocity.y += (force.y/body->mass)*step;
   body->velocity.z += (force.z/body->mass)*step;
   if (!isfinite(body->position.x) || !isfinite(body->position.y) ||
       !isfinite(body->position.z) || !isfinite(body->velocity.x) ||
       !isfinite(body->velocity.y) || !isfinite(body->velocity.z)) {
      fprintf(stderr, "Non-finite state; check input and time step\n");
      exit(EXIT_FAILURE);
   }
}

// Номер шага задаёт время без накопления погрешности от повторного сложения dt.
double Next_time(double time, size_t steps, const struct input_s* input) {
   double next = ((double)steps + 1.0)*input->time_step;
   double tolerance = 4.0*DBL_EPSILON*input->end_time;

   // Остаток в пределах точности double объединяется с последним шагом.
   if (next >= input->end_time || input->end_time - next <= tolerance)
      next = input->end_time;
   if (next <= time) {
      fprintf(stderr, "Time step is too small to advance time\n");
      exit(EXIT_FAILURE);
   }
   return next;
}
