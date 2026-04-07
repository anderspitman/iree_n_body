#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "iree/base/tracing.h"
#include "iree/runtime/api.h"

#define BODY_STATE_WIDTH 5
#define TRAIL_LENGTH 24
#define DEFAULT_BODY_COUNT 3
#define DEFAULT_STEPS 0L
#define DEFAULT_TIME_STEP 0.040f
#define DEFAULT_GRAVITY 3.0f
#define DEFAULT_SOFTENING 0.0001f
#define DEFAULT_RENDER_HZ 60.0

struct screen_size {
  int width;
  int height;
};

struct body {
  float x;
  float y;
  float vx;
  float vy;
  float mass;
  int trail_x[TRAIL_LENGTH];
  int trail_y[TRAIL_LENGTH];
  int trail_count;
  int trail_next;
};

struct sim_options {
  long steps;
  int headless;
  const char* backend;
  const char* bodies_file_path;
  const char* vmfb_path;
};

struct runtime_state {
  iree_runtime_instance_t* instance;
  iree_runtime_session_t* session;
};

static volatile sig_atomic_t keep_running = 1;
static char g_vmfb_path[1024];

static int is_supported_backend(const char* backend) {
  return strcmp(backend, "local-task") == 0 ||
         strcmp(backend, "local-sync") == 0 ||
         strcmp(backend, "vulkan") == 0 || strcmp(backend, "cuda") == 0;
}

static void handle_signal(int signum) {
  (void)signum;
  keep_running = 0;
}

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void sleep_seconds(double seconds) {
  struct timespec request;
  struct timespec remaining;

  if (seconds <= 0.0) {
    return;
  }

  request.tv_sec = (time_t)seconds;
  request.tv_nsec = (long)((seconds - (double)request.tv_sec) * 1000000000.0);
  if (request.tv_nsec < 0L) {
    request.tv_nsec = 0L;
  }

  while (nanosleep(&request, &remaining) != 0) {
    if (errno != EINTR || !keep_running) {
      break;
    }
    request = remaining;
  }
}

static void move_cursor(int row, int column) {
  printf("\033[%d;%dH", row, column);
}

static struct screen_size detect_screen_size(void) {
  struct screen_size size;
  struct winsize ws;

  size.width = 80;
  size.height = 24;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
    if (ws.ws_col > 0) {
      size.width = (int)ws.ws_col;
    }
    if (ws.ws_row > 0) {
      size.height = (int)ws.ws_row;
    }
  }

  if (size.width < 20) {
    size.width = 20;
  }
  if (size.height < 10) {
    size.height = 10;
  }
  return size;
}

static void write_braille_cell(unsigned int mask) {
  unsigned int codepoint;
  char utf8[4];

  if (mask == 0U) {
    putchar(' ');
    return;
  }

  codepoint = 0x2800U + mask;
  utf8[0] = (char)(0xE0U | ((codepoint >> 12) & 0x0FU));
  utf8[1] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
  utf8[2] = (char)(0x80U | (codepoint & 0x3FU));
  utf8[3] = '\0';
  fputs(utf8, stdout);
}

static void set_braille_dot(unsigned int* cells, const struct screen_size* screen,
                            int sub_x, int sub_y) {
  static const unsigned int dot_bits[4][2] = {{0x01U, 0x08U},
                                              {0x02U, 0x10U},
                                              {0x04U, 0x20U},
                                              {0x40U, 0x80U}};
  int cell_x;
  int cell_y;
  int dot_x;
  int dot_y;

  if (sub_x < 0 || sub_x >= screen->width * 2 || sub_y < 0 ||
      sub_y >= screen->height * 4) {
    return;
  }

  cell_x = sub_x / 2;
  cell_y = sub_y / 4;
  dot_x = sub_x % 2;
  dot_y = sub_y % 4;
  cells[cell_y * screen->width + cell_x] |= dot_bits[dot_y][dot_x];
}

static void draw_braille_line(unsigned int* cells, const struct screen_size* screen,
                              int x0, int y0, int x1, int y1) {
  int dx;
  int dy;
  int sx;
  int sy;
  int err;

  dx = (x0 < x1) ? (x1 - x0) : (x0 - x1);
  dy = (y0 < y1) ? (y1 - y0) : (y0 - y1);
  sx = (x0 < x1) ? 1 : -1;
  sy = (y0 < y1) ? 1 : -1;
  err = dx - dy;

  for (;;) {
    int twice_err;
    set_braille_dot(cells, screen, x0, y0);
    if (x0 == x1 && y0 == y1) {
      break;
    }
    twice_err = err * 2;
    if (twice_err > -dy) {
      err -= dy;
      x0 += sx;
    }
    if (twice_err < dx) {
      err += dx;
      y0 += sy;
    }
  }
}

static void draw_braille_circle(unsigned int* cells,
                                const struct screen_size* screen, int center_x,
                                int center_y, int radius) {
  int dy;
  int radius_sq;

  radius_sq = radius * radius;
  for (dy = -radius; dy <= radius; ++dy) {
    int dx_limit_sq;

    dx_limit_sq = radius_sq - dy * dy;
    if (dx_limit_sq >= 0) {
      int dx_limit;
      int dx;

      dx_limit = (int)(sqrt((double)dx_limit_sq) + 0.5);
      for (dx = -dx_limit; dx <= dx_limit; ++dx) {
        set_braille_dot(cells, screen, center_x + dx, center_y + dy);
      }
    }
  }
}

static void body_to_subcell(const struct body* body,
                            const struct screen_size* screen, double scale,
                            int* sub_x, int* sub_y) {
  *sub_x = (int)((screen->width * 0.5 + (double)body->x * scale) * 2.0 + 0.5);
  *sub_y =
      (int)((screen->height * 0.5 + (double)body->y * scale) * 4.0 + 0.5);

  if (*sub_x < 0) {
    *sub_x = 0;
  } else if (*sub_x > screen->width * 2 - 1) {
    *sub_x = screen->width * 2 - 1;
  }
  if (*sub_y < 0) {
    *sub_y = 0;
  } else if (*sub_y > screen->height * 4 - 1) {
    *sub_y = screen->height * 4 - 1;
  }
}

static void push_trail(struct body* body, const struct screen_size* screen,
                       double scale) {
  int sub_x;
  int sub_y;
  int last_index;

  body_to_subcell(body, screen, scale, &sub_x, &sub_y);
  if (body->trail_count > 0) {
    last_index = body->trail_next - 1;
    if (last_index < 0) {
      last_index += TRAIL_LENGTH;
    }
    if (body->trail_x[last_index] == sub_x && body->trail_y[last_index] == sub_y) {
      return;
    }
  }

  body->trail_x[body->trail_next] = sub_x;
  body->trail_y[body->trail_next] = sub_y;
  body->trail_next = (body->trail_next + 1) % TRAIL_LENGTH;
  if (body->trail_count < TRAIL_LENGTH) {
    body->trail_count += 1;
  }
}

static void render_scene(const struct body* bodies, int body_count,
                         const struct screen_size* screen, double scale,
                         long step_index, const char* backend) {
  unsigned int* cells;
  float max_mass;
  int i;
  int j;

  cells = (unsigned int*)calloc((size_t)(screen->width * screen->height),
                                sizeof(unsigned int));
  if (cells == NULL) {
    return;
  }

  max_mass = 0.0f;
  for (i = 0; i < body_count; ++i) {
    if (bodies[i].mass > max_mass) {
      max_mass = bodies[i].mass;
    }
  }
  if (max_mass <= 0.0f) {
    max_mass = 1.0f;
  }

  for (i = 0; i < body_count; ++i) {
    for (j = 0; j + 1 < bodies[i].trail_count; ++j) {
      int index0;
      int index1;

      index0 = bodies[i].trail_next - bodies[i].trail_count + j;
      if (index0 < 0) {
        index0 += TRAIL_LENGTH;
      }
      index1 = index0 + 1;
      if (index1 >= TRAIL_LENGTH) {
        index1 = 0;
      }

      draw_braille_line(cells, screen, bodies[i].trail_x[index0],
                        bodies[i].trail_y[index0], bodies[i].trail_x[index1],
                        bodies[i].trail_y[index1]);
    }
  }

  for (i = 0; i < body_count; ++i) {
    int center_x;
    int center_y;
    int radius;

    body_to_subcell(&bodies[i], screen, scale, &center_x, &center_y);
    radius = 1 + (int)(2.0f * sqrtf(bodies[i].mass / max_mass));
    if (radius < 0.1) {
      radius = 0.1;
    }
    draw_braille_circle(cells, screen, center_x, center_y, radius);
  }

  move_cursor(1, 1);
  for (i = 0; i < screen->height - 1; ++i) {
    for (j = 0; j < screen->width; ++j) {
      write_braille_cell(cells[i * screen->width + j]);
    }
    putchar('\n');
  }
  printf("step=%ld  bodies=%d  backend=%s  tracy=enabled  Ctrl-C quits",
         step_index, body_count, backend);
  fflush(stdout);

  free(cells);
}

static void initialize_default_bodies(struct body* bodies,
                                      const struct screen_size* screen,
                                      double scale) {
  int i;
  const float figure8_velocity_scale = (float)sqrt(DEFAULT_GRAVITY);

  memset(bodies, 0, sizeof(struct body) * (size_t)DEFAULT_BODY_COUNT);

  /* Canonical equal-mass three-body figure-eight initial conditions. */
  bodies[0].x = -0.97000436f;
  bodies[0].y = 0.24308753f;
  bodies[0].vx = 0.46620369f * figure8_velocity_scale;
  bodies[0].vy = 0.43236573f * figure8_velocity_scale;
  bodies[0].mass = 1.0f;

  bodies[1].x = 0.97000436f;
  bodies[1].y = -0.24308753f;
  bodies[1].vx = 0.46620369f * figure8_velocity_scale;
  bodies[1].vy = 0.43236573f * figure8_velocity_scale;
  bodies[1].mass = 1.0f;

  bodies[2].x = 0.0f;
  bodies[2].y = 0.0f;
  bodies[2].vx = -0.93240737f * figure8_velocity_scale;
  bodies[2].vy = -0.86473146f * figure8_velocity_scale;
  bodies[2].mass = 1.0f;

  for (i = 0; i < DEFAULT_BODY_COUNT; ++i) {
    push_trail(&bodies[i], screen, scale);
  }
}

static void pack_bodies(const struct body* bodies, int body_count, float* state) {
  int i;
  for (i = 0; i < body_count; ++i) {
    int base;
    base = i * BODY_STATE_WIDTH;
    state[base + 0] = bodies[i].x;
    state[base + 1] = bodies[i].y;
    state[base + 2] = bodies[i].vx;
    state[base + 3] = bodies[i].vy;
    state[base + 4] = bodies[i].mass;
  }
}

static void unpack_bodies(struct body* bodies, int body_count, const float* state,
                          const struct screen_size* screen, double scale) {
  int i;
  for (i = 0; i < body_count; ++i) {
    int base;
    base = i * BODY_STATE_WIDTH;
    bodies[i].x = state[base + 0];
    bodies[i].y = state[base + 1];
    bodies[i].vx = state[base + 2];
    bodies[i].vy = state[base + 3];
    bodies[i].mass = state[base + 4];
    push_trail(&bodies[i], screen, scale);
  }
}

static int parse_long(const char* text, long* out_value) {
  char* end;
  long value;

  errno = 0;
  value = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    return 0;
  }
  *out_value = value;
  return 1;
}

static int parse_options(int argc, char** argv, struct sim_options* options) {
  int i;

  options->steps = DEFAULT_STEPS;
  options->headless = 0;
  options->backend = "local-task";
  options->bodies_file_path = NULL;
  options->vmfb_path = NULL;

  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--headless") == 0) {
      options->headless = 1;
    } else if (strcmp(argv[i], "--backend") == 0) {
      if (i + 1 >= argc || !is_supported_backend(argv[i + 1])) {
        return 0;
      }
      options->backend = argv[i + 1];
      ++i;
    } else if (strcmp(argv[i], "--bodies-file") == 0) {
      if (i + 1 >= argc) {
        return 0;
      }
      options->bodies_file_path = argv[i + 1];
      ++i;
    } else if (strcmp(argv[i], "--vmfb") == 0) {
      if (i + 1 >= argc) {
        return 0;
      }
      options->vmfb_path = argv[i + 1];
      ++i;
    } else if (strcmp(argv[i], "--steps") == 0) {
      if (i + 1 >= argc || !parse_long(argv[i + 1], &options->steps)) {
        return 0;
      }
      ++i;
    } else if (!parse_long(argv[i], &options->steps)) {
      return 0;
    }
  }
  return 1;
}

static int load_bodies_from_file(const char* path, struct body** out_bodies,
                                 int* out_body_count,
                                 const struct screen_size* screen,
                                 double scale) {
  FILE* file;
  struct body* bodies;
  int capacity;
  int count;

  file = fopen(path, "r");
  if (file == NULL) {
    return 0;
  }

  capacity = 8;
  count = 0;
  bodies = (struct body*)calloc((size_t)capacity, sizeof(struct body));
  if (bodies == NULL) {
    fclose(file);
    return 0;
  }

  for (;;) {
    float x;
    float y;
    float vx;
    float vy;
    float mass;
    int scan_count;

    scan_count = fscanf(file, " %f %f %f %f %f", &x, &y, &vx, &vy, &mass);
    if (scan_count == EOF) {
      break;
    }
    if (scan_count != BODY_STATE_WIDTH) {
      free(bodies);
      fclose(file);
      return 0;
    }

    if (count == capacity) {
      int new_capacity;
      struct body* new_bodies;

      new_capacity = capacity * 2;
      new_bodies =
          (struct body*)realloc(bodies, sizeof(struct body) * (size_t)new_capacity);
      if (new_bodies == NULL) {
        free(bodies);
        fclose(file);
        return 0;
      }
      memset(new_bodies + capacity, 0,
             sizeof(struct body) * (size_t)(new_capacity - capacity));
      bodies = new_bodies;
      capacity = new_capacity;
    }

    memset(&bodies[count], 0, sizeof(struct body));
    bodies[count].x = x;
    bodies[count].y = y;
    bodies[count].vx = vx;
    bodies[count].vy = vy;
    bodies[count].mass = mass;
    push_trail(&bodies[count], screen, scale);
    ++count;
  }

  fclose(file);
  if (count <= 0) {
    free(bodies);
    return 0;
  }

  *out_bodies = bodies;
  *out_body_count = count;
  return 1;
}

static int file_exists(const char* path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static void directory_from_path(const char* path, char* out_dir, size_t out_size) {
  size_t i;
  size_t last_slash;

  last_slash = 0;
  out_dir[0] = '.';
  out_dir[1] = '\0';
  for (i = 0; path[i] != '\0'; ++i) {
    if (path[i] == '/') {
      last_slash = i + 1;
    }
  }
  if (last_slash == 0) {
    return;
  }
  if (last_slash >= out_size) {
    last_slash = out_size - 1;
  }
  memcpy(out_dir, path, last_slash);
  out_dir[last_slash] = '\0';
  if (last_slash > 1 && out_dir[last_slash - 1] == '/') {
    out_dir[last_slash - 1] = '\0';
  }
}

static void detect_binary_dir(const char* argv0, char* out_dir, size_t out_size) {
  ssize_t length;
  char exe_path[1024];

  length = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
  if (length > 0) {
    exe_path[length] = '\0';
    directory_from_path(exe_path, out_dir, out_size);
    return;
  }
  directory_from_path(argv0, out_dir, out_size);
}

static int join_path(char* out_path, size_t out_size, const char* left,
                     const char* right) {
  size_t left_len;
  size_t right_len;
  size_t total_len;

  left_len = strlen(left);
  right_len = strlen(right);
  total_len = left_len + 1 + right_len + 1;
  if (total_len > out_size) {
    return 0;
  }
  memcpy(out_path, left, left_len);
  out_path[left_len] = '/';
  memcpy(out_path + left_len + 1, right, right_len);
  out_path[left_len + 1 + right_len] = '\0';
  return 1;
}

static int locate_vmfb(const char* argv0, const struct sim_options* options) {
  char binary_dir[1024];
  char cwd_vmfb_path[1024];
  char bin_vmfb_path[1024];
  char backend_vmfb_name[1024];
  char cwd_backend_vmfb_path[1024];
  char bin_backend_vmfb_path[1024];

  if (options->vmfb_path != NULL) {
    if (file_exists(options->vmfb_path)) {
      strcpy(g_vmfb_path, options->vmfb_path);
      return 1;
    }
    return 0;
  }

  detect_binary_dir(argv0, binary_dir, sizeof(binary_dir));
  snprintf(cwd_vmfb_path, sizeof(cwd_vmfb_path), "n_body.vmfb");
  snprintf(backend_vmfb_name, sizeof(backend_vmfb_name), "n_body_%s.vmfb",
           options->backend);
  snprintf(cwd_backend_vmfb_path, sizeof(cwd_backend_vmfb_path), "%s",
           backend_vmfb_name);
  if (!join_path(bin_vmfb_path, sizeof(bin_vmfb_path), binary_dir,
                 "n_body.vmfb")) {
    bin_vmfb_path[0] = '\0';
  }
  if (!join_path(bin_backend_vmfb_path, sizeof(bin_backend_vmfb_path),
                 binary_dir, backend_vmfb_name)) {
    bin_backend_vmfb_path[0] = '\0';
  }

  if (file_exists(cwd_backend_vmfb_path)) {
    strcpy(g_vmfb_path, cwd_backend_vmfb_path);
    return 1;
  }
  if (bin_backend_vmfb_path[0] != '\0' && file_exists(bin_backend_vmfb_path)) {
    strcpy(g_vmfb_path, bin_backend_vmfb_path);
    return 1;
  }

  if (file_exists(cwd_vmfb_path)) {
    strcpy(g_vmfb_path, cwd_vmfb_path);
    return 1;
  }
  if (bin_vmfb_path[0] != '\0' && file_exists(bin_vmfb_path)) {
    strcpy(g_vmfb_path, bin_vmfb_path);
    return 1;
  }
  return 0;
}

static iree_status_t append_input_state(iree_runtime_call_t* call,
                                        iree_runtime_session_t* session,
                                        const float* state, int body_count) {
  iree_hal_buffer_view_t* input_view;
  const iree_hal_dim_t shape[2] = {(iree_hal_dim_t)body_count,
                                   (iree_hal_dim_t)BODY_STATE_WIDTH};
  iree_hal_buffer_params_t params;

  input_view = NULL;
  memset(&params, 0, sizeof(params));
  params.type = IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL;
  params.access = IREE_HAL_MEMORY_ACCESS_ALL;
  params.usage = IREE_HAL_BUFFER_USAGE_DEFAULT;

  IREE_RETURN_IF_ERROR(iree_hal_buffer_view_allocate_buffer_copy(
      iree_runtime_session_device(session),
      iree_runtime_session_device_allocator(session), IREE_ARRAYSIZE(shape), shape,
      IREE_HAL_ELEMENT_TYPE_FLOAT_32, IREE_HAL_ENCODING_TYPE_DENSE_ROW_MAJOR,
      params, iree_make_const_byte_span((void*)state,
                                        (iree_host_size_t)(sizeof(float) * body_count *
                                                           BODY_STATE_WIDTH)),
      &input_view));
  {
    iree_status_t status;
    status = iree_runtime_call_inputs_push_back_buffer_view(call, input_view);
    iree_hal_buffer_view_release(input_view);
    return status;
  }
}

static iree_status_t append_input_scalar(iree_runtime_call_t* call, float value) {
  iree_vm_value_t scalar_value;
  scalar_value = iree_vm_value_make_f32(value);
  return iree_vm_list_push_value(iree_runtime_call_inputs(call), &scalar_value);
}

static iree_status_t simulation_step(iree_runtime_session_t* session,
                                     const float* in_state, int body_count,
                                     float gravity, float softening, float dt,
                                     float* out_state) {
  iree_runtime_call_t call;
  iree_hal_buffer_view_t* output_view;
  iree_status_t status;

  output_view = NULL;
  IREE_RETURN_IF_ERROR(iree_runtime_call_initialize_by_name(
      session, iree_make_cstring_view("module.step"), &call));

  status = append_input_state(&call, session, in_state, body_count);
  if (iree_status_is_ok(status)) {
    status = append_input_scalar(&call, gravity);
  }
  if (iree_status_is_ok(status)) {
    status = append_input_scalar(&call, softening);
  }
  if (iree_status_is_ok(status)) {
    status = append_input_scalar(&call, dt);
  }
  if (iree_status_is_ok(status)) {
    status = iree_runtime_call_invoke(&call, 0);
  }
  if (iree_status_is_ok(status)) {
    status = iree_runtime_call_outputs_pop_front_buffer_view(&call, &output_view);
  }
  if (iree_status_is_ok(status)) {
    status = iree_hal_device_transfer_d2h(
        iree_runtime_session_device(session),
        iree_hal_buffer_view_buffer(output_view), 0, out_state,
        (iree_device_size_t)(sizeof(float) * body_count * BODY_STATE_WIDTH),
        IREE_HAL_TRANSFER_BUFFER_FLAG_DEFAULT, iree_infinite_timeout());
  }

  iree_hal_buffer_view_release(output_view);
  iree_runtime_call_deinitialize(&call);
  return status;
}

static iree_status_t initialize_runtime(struct runtime_state* runtime,
                                        const char* backend) {
  iree_runtime_instance_options_t instance_options;
  iree_runtime_session_options_t session_options;
  iree_hal_device_t* device;
  iree_status_t status;

  device = NULL;
  runtime->instance = NULL;
  runtime->session = NULL;

  iree_runtime_instance_options_initialize(&instance_options);
  iree_runtime_instance_options_use_all_available_drivers(&instance_options);
  status = iree_runtime_instance_create(&instance_options, iree_allocator_system(),
                                        &runtime->instance);
  if (iree_status_is_ok(status)) {
    status = iree_runtime_instance_try_create_default_device(
        runtime->instance, iree_make_cstring_view(backend), &device);
  }
  if (iree_status_is_ok(status)) {
    iree_runtime_session_options_initialize(&session_options);
    status = iree_runtime_session_create_with_device(
        runtime->instance, &session_options, device,
        iree_runtime_instance_host_allocator(runtime->instance),
        &runtime->session);
  }
  iree_hal_device_release(device);

  if (iree_status_is_ok(status)) {
    status =
        iree_runtime_session_append_bytecode_module_from_file(runtime->session,
                                                              g_vmfb_path);
  }

  if (!iree_status_is_ok(status)) {
    if (runtime->session != NULL) {
      iree_runtime_session_release(runtime->session);
      runtime->session = NULL;
    }
    if (runtime->instance != NULL) {
      iree_runtime_instance_release(runtime->instance);
      runtime->instance = NULL;
    }
  }
  return status;
}

static void deinitialize_runtime(struct runtime_state* runtime) {
  if (runtime->session != NULL) {
    iree_runtime_session_release(runtime->session);
    runtime->session = NULL;
  }
  if (runtime->instance != NULL) {
    iree_runtime_instance_release(runtime->instance);
    runtime->instance = NULL;
  }
}

static void print_final_state(const struct body* bodies, int body_count,
                              long step_index) {
  int i;
  printf("final_state steps=%ld\n", step_index);
  for (i = 0; i < body_count; ++i) {
    printf("%d %.6f %.6f %.6f %.6f %.6f\n", i, (double)bodies[i].x,
           (double)bodies[i].y, (double)bodies[i].vx, (double)bodies[i].vy,
           (double)bodies[i].mass);
  }
}

int main(int argc, char** argv) {
  struct sim_options options;
  struct screen_size screen;
  struct runtime_state runtime;
  struct body* bodies;
  float* state_in;
  float* state_out;
  int body_count;
  double scale;
  double render_interval;
  double previous_time;
  double render_accumulator;
  long step_index;
  int exit_code;
  iree_status_t status;

  IREE_TRACE_APP_ENTER();
  bodies = NULL;
  state_in = NULL;
  state_out = NULL;
  body_count = 0;

  if (!parse_options(argc, argv, &options)) {
    fprintf(stderr,
            "usage: %s [--steps N|N] [--headless] [--backend BACKEND] [--bodies-file PATH] [--vmfb PATH]\n",
            argv[0]);
    IREE_TRACE_APP_EXIT(1);
    return 1;
  }

  if (!locate_vmfb(argv[0], &options)) {
    fprintf(stderr,
            "could not locate a VMFB for backend '%s'; expected n_body_%s.vmfb or n_body.vmfb in the current directory or next to the executable\n",
            options.backend, options.backend);
    IREE_TRACE_APP_EXIT(1);
    return 1;
  }

  screen = detect_screen_size();
  scale = ((screen.width < screen.height) ? screen.width : screen.height) * 0.30;
  render_interval = 1.0 / DEFAULT_RENDER_HZ;
  if (options.bodies_file_path != NULL) {
    if (!load_bodies_from_file(options.bodies_file_path, &bodies, &body_count,
                               &screen, scale)) {
      fprintf(stderr,
              "failed to load body state from '%s' as whitespace-delimited x y vx vy mass rows\n",
              options.bodies_file_path);
      IREE_TRACE_APP_EXIT(1);
      return 1;
    }
  } else {
    body_count = DEFAULT_BODY_COUNT;
    bodies = (struct body*)calloc((size_t)body_count, sizeof(struct body));
    if (bodies == NULL) {
      fprintf(stderr, "failed to allocate default body state\n");
      IREE_TRACE_APP_EXIT(1);
      return 1;
    }
    initialize_default_bodies(bodies, &screen, scale);
  }

  state_in =
      (float*)malloc(sizeof(float) * (size_t)body_count * BODY_STATE_WIDTH);
  state_out =
      (float*)malloc(sizeof(float) * (size_t)body_count * BODY_STATE_WIDTH);
  if (state_in == NULL || state_out == NULL) {
    fprintf(stderr, "failed to allocate simulation buffers for %d bodies\n",
            body_count);
    free(state_in);
    free(state_out);
    free(bodies);
    IREE_TRACE_APP_EXIT(1);
    return 1;
  }

  status = initialize_runtime(&runtime, options.backend);
  if (!iree_status_is_ok(status)) {
    iree_status_fprint(stderr, status);
    iree_status_ignore(status);
    free(state_in);
    free(state_out);
    free(bodies);
    IREE_TRACE_APP_EXIT(1);
    return 1;
  }

  signal(SIGINT, handle_signal);
  exit_code = 0;
  step_index = 0;
  previous_time = now_seconds();
  render_accumulator = render_interval;

  if (!options.headless) {
    printf("\033[2J\033[H\033[?25l");
    fflush(stdout);
  }

  while (keep_running && (options.steps <= 0 || step_index < options.steps)) {
    pack_bodies(bodies, body_count, state_in);
    status = simulation_step(runtime.session, state_in, body_count,
                             DEFAULT_GRAVITY, DEFAULT_SOFTENING,
                             DEFAULT_TIME_STEP, state_out);
    if (!iree_status_is_ok(status)) {
      iree_status_fprint(stderr, status);
      iree_status_ignore(status);
      exit_code = 1;
      break;
    }

    unpack_bodies(bodies, body_count, state_out, &screen, scale);
    ++step_index;

    if (!options.headless) {
      double current_time;
      double elapsed;

      current_time = now_seconds();
      elapsed = current_time - previous_time;
      previous_time = current_time;
      if (elapsed < 0.0) {
        elapsed = 0.0;
      } else if (elapsed > 0.25) {
        elapsed = 0.25;
      }
      render_accumulator += elapsed;
      if (render_accumulator >= render_interval) {
        printf("\033[2J\033[H");
        render_scene(bodies, body_count, &screen, scale, step_index,
                     options.backend);
        render_accumulator = 0.0;
      }
      if (keep_running && render_accumulator < render_interval) {
        sleep_seconds(render_interval - render_accumulator);
      }
    }
  }

  if (!options.headless) {
    move_cursor(screen.height, 1);
    printf("\033[?25h\n");
    fflush(stdout);
  }
  print_final_state(bodies, body_count, step_index);
  deinitialize_runtime(&runtime);
  free(state_in);
  free(state_out);
  free(bodies);

  IREE_TRACE_APP_EXIT(exit_code);
  return exit_code;
}
