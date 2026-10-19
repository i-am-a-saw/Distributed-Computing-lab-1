#include <stdio.h>
#include <stdlib.h>
#include "nbody.h"
#include "timer.h"

void Compute_forces(const struct body_s bodies[], size_t body_count,
                    struct vector_s forces[]);

// Один последовательный расчёт: чтение данных, шаги Эйлера и запись траектории.
int main(int argc, char* argv[]) {
   struct input_s input;
   struct body_s* bodies;
   struct vector_s* forces;
   FILE* output;
   size_t body_count, i, steps = 0;
   double time = 0.0, next, step;
   double start, finish, total_start, total_finish, compute_time = 0.0;

   Get_options(argc, argv, 0, &input);
   bodies = Read_bodies(input.input_name, &body_count);
   forces = Allocate(body_count, sizeof(*forces));
   output = Open_output(&input, body_count);

   // Подготовка данных закончена; общее время включает запись состояний в CSV.
   GET_TIME(total_start);
   Write_state(output, time, bodies, body_count, input.output_3d);
   while (time < input.end_time) {
      next = Next_time(time, steps, &input);
      step = next - time;
      // В вычислительное время входят только силы и обновление тел.
      GET_TIME(start);
      Compute_forces(bodies, body_count, forces);
      // Все силы уже найдены по прежним координатам, можно менять состояния.
      for (i = 0; i < body_count; i++) Update_body(&bodies[i], forces[i], step);
      GET_TIME(finish);
      compute_time += finish - start;
      time = next;
      steps++;
      Write_state(output, time, bodies, body_count, input.output_3d);
   }
   Close_output(output);
   GET_TIME(total_finish);
   printf("Bodies = %zu, steps = %zu\n", body_count, steps);
   printf("Compute time = %e seconds\n", compute_time);
   printf("Total time (with CSV) = %e seconds\n", total_finish - total_start);
   free(forces);
   free(bodies);
   return 0;
}

// Каждая пара учитывается один раз: силы на тела равны и противоположны.
void Compute_forces(const struct body_s bodies[], size_t body_count,
                    struct vector_s forces[]) {
   size_t i, j;
   struct vector_s zero = {0.0, 0.0, 0.0};
   for (i = 0; i < body_count; i++) forces[i] = zero;
   for (i = 0; i < body_count; i++) {
      for (j = i + 1; j < body_count; j++) {
         struct vector_s force = Pair_force(&bodies[i], &bodies[j]);
         Add_force(&forces[i], force);
         Subtract_force(&forces[j], force);
      }
   }
}
