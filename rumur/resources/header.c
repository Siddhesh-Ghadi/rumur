#ifndef __OPTIMIZE__
  #ifdef __clang__
    #ifdef __x86_64__
      #warning you are compiling without optimizations enabled. I would suggest -march=native -O3 -mcx16.
    #else
      #warning you are compiling without optimizations enabled. I would suggest -march=native -O3.
    #endif
  #else
    #ifdef __x86_64__
      #warning you are compiling without optimizations enabled. I would suggest -march=native -O3 -fwhole-program -mcx16.
    #else
      #warning you are compiling without optimizations enabled. I would suggest -march=native -O3 -fwhole-program.
    #endif
  #endif
#endif

/* Generic support for maximum and minimum values of types. This is useful for,
 * e.g. size_t, where we don't properly have SIZE_MAX and SIZE_MIN.
 */
#define MIN(type) _Generic((type)1,                                            \
  int8_t:   INT8_MIN,                                                          \
  int16_t:  INT16_MIN,                                                         \
  int32_t:  INT32_MIN,                                                         \
  int64_t:  INT64_MIN,                                                         \
  uint8_t:  (uint8_t)0,                                                        \
  uint16_t: (uint16_t)0,                                                       \
  uint32_t: (uint32_t)0,                                                       \
  uint64_t: (uint64_t)0)
#define MAX(type) _Generic((type)1,                                            \
  int8_t:   INT8_MAX,                                                          \
  int16_t:  INT16_MAX,                                                         \
  int32_t:  INT32_MAX,                                                         \
  int64_t:  INT64_MAX,                                                         \
  uint8_t:  UINT8_MAX,                                                         \
  uint16_t: UINT16_MAX,                                                        \
  uint32_t: UINT32_MAX,                                                        \
  uint64_t: UINT64_MAX)

/* Abstraction over the type we use for scalar values. Other code should be
 * agnostic to what the underlying type is, so if you are porting this code to a
 * future platform where you need a wider type, modifying these lines should be
 * enough.
 */
typedef int64_t value_t;
#define PRIVAL PRId64
#define VALUE_MAX INT64_MAX
#define VALUE_MIN INT64_MIN
#define VALUE_C INT64_C

/* XXX: intypes.h does not seem to give us this. */
#ifndef SIZE_C
  #define SIZE_C(x) _Generic((size_t)1,                                        \
    unsigned: x ## u,                                                          \
    unsigned long: x ## ul,                                                    \
    unsigned long long: x ## ull)
#endif

/* A more powerful assert that treats the assertion as an assumption when
 * assertions are disabled.
 */
#ifndef NDEBUG
  #define ASSERT(expr) assert(expr)
#else
  #define ASSERT(expr) \
    do { \
      /* The following is an idiom for teaching the compiler an assumption. */ \
      if (!(expr)) { \
        __builtin_unreachable(); \
      } \
    } while (0)
#endif

#define BITS_TO_BYTES(size) (size / 8 + (size % 8 == 0 ? 0 : 1))

/* The size of the compressed state data in bytes. */
enum { STATE_SIZE_BYTES = BITS_TO_BYTES(STATE_SIZE_BITS) };

/* A word about atomics... There are three different atomic operation mechanisms
 * used in this code and it may not immediately be obvious why one was not
 * sufficient. The three are:
 *
 *   1. C11 atomics: used for variables that are consistently accessed with
 *      atomic semantics. This mechanism is simple, concise and standardised.
 *   2. GCC __atomic built-ins: Used for variables that are sometimes accessed
 *      with atomic semantics and sometimes as regular memory operations. The
 *      C11 atomics cannot give us this and the __atomic built-ins are
 *      implemented by the major compilers.
 *   3. GCC __sync built-ins: used for 128-bit atomic accesses on x86-64. It
 *      seems the __atomic built-ins do not result in a CMPXCHG instruction, but
 *      rather in a less efficient library call. See
 *      https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80878.
 */

/* Identifier of the current thread. This counts up from 0 and thus is suitable
 * to use for, e.g., indexing into arrays. The initial thread has ID 0.
 */
static _Thread_local size_t thread_id;

/* The threads themselves. Note that we have no element for the initial thread,
 * so *your* thread is 'threads[thread_id - 1]'.
 */
static pthread_t threads[THREADS - 1];

/* What we are currently doing. Either "warming up" (running single threaded
 * building up queue occupancy) or "free running" (running multithreaded).
 */
static enum { WARMUP, RUN } phase = WARMUP;

/* Number of errors we've noted so far. If a thread sees this hit or exceed
 * MAX_ERRORS, they should attempt to exit gracefully as soon as possible.
 */
static atomic_ulong error_count;

/* Number of rules that have been processed. There are two representations of
 * this: a thread-local count of how many rules we have fired thus far and a
 * global array of *final* counts of fired rules per-thread that is updated and
 * used as threads are exiting. The purpose of this duplication is to let the
 * compiler layout the thread-local variable in a cache-friendly way and use
 * this during checking, rather than having all threads contending on the global
 * array whose entries are likely all within the same cache line.
 */
static _Thread_local uintmax_t rules_fired_local;
static uintmax_t rules_fired[THREADS];

/* Checkpoint to restore to after reporting an error. This is only used if we
 * are tolerating more than one error before exiting.
 */
static _Thread_local jmp_buf checkpoint;

static_assert(MAX_ERRORS > 0, "illegal MAX_ERRORS value");

/* Whether we need to save and restore checkpoints. This is determined by
 * whether we ever need to perform the action "discard the current state and
 * skip to checking the next." This scenario can occur for two reasons:
 *   1. We are running multithreaded, have just found an error and have not yet
 *      hit MAX_ERRORS. In this case we want to longjmp back to resume checking.
 *   2. We failed an assumption. In this case we want to mark the current state
 *      as invalid and resume checking with the next state.
 * In either scenario the actual longjmp performed is the same, but by knowing
 * statically whether either can occur we can avoid calling setjmp if both are
 * impossible.
 */
enum { JMP_BUF_NEEDED = MAX_ERRORS > 1 || ASSUMPTION_COUNT > 0};

/*******************************************************************************
 * Sandbox support.                                                            *
 *                                                                             *
 * Because we're running generated code, it seems wise to use OS mechanisms to *
 * reduce our privileges, where possible.                                      *
 ******************************************************************************/

static void sandbox(void) {

  if (!SANDBOX_ENABLED) {
    return;
  }

#ifdef __APPLE__
  {
    char *err;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    int r = sandbox_init(kSBXProfilePureComputation, SANDBOX_NAMED, &err);
#pragma clang diagnostic pop

    if (r != 0) {
      fprintf(stderr, "sandbox_init failed: %s\n", err);
      free(err);
      exit(EXIT_FAILURE);
    }

    return;
  }
#endif

#if defined(__linux__)
  #if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
  {
    /* Disable the addition of new privileges via execve and friends. */
    int r = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    if (r != 0) {
      perror("prctl(PR_SET_NO_NEW_PRIVS) failed");
      exit(EXIT_FAILURE);
    }

    /* A BPF program that traps on any syscall we want to disallow. */
    static struct sock_filter filter[] = {

#if 0
      // TODO: The following will require some pesky ifdef mess because the
      // Linux headers don't seem to define a "current architecture" constant.
      /* Validate that we're running on the same architecture we were compiled
       * for. If not, the syscall numbers we're using may be wrong.
       */
      BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, arch)),
      BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, ARCH_NR, 1, 0),
      BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),
#endif

      /* Load syscall number. */
      BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, nr)),

      /* Enable exiting. */
      BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, __NR_exit_group, 0, 1),
      BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),

      /* Enable syscalls used by printf. */
      BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, __NR_fstat, 0, 1),
      BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
      BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, __NR_write, 0, 1),
      BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),

      /* Enable syscalls used by malloc. */
      BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, __NR_brk, 0, 1),
      BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
      BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, __NR_mmap, 0, 1),
      BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
      BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, __NR_munmap, 0, 1),
      BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),

      /* Enable ioctl which will be used by isatty.
       * TODO: lock this down to a specific ioctl number.
       */
      BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, __NR_ioctl, 0, 1),
      BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),

      /* If we're running multithreaded, enable syscalls that used by pthreads.
       */
      BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, __NR_clone, 0, 1),
      BPF_STMT(BPF_RET|BPF_K, THREADS > 1 ? SECCOMP_RET_ALLOW : SECCOMP_RET_TRAP),
      BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, __NR_close, 0, 1),
      BPF_STMT(BPF_RET|BPF_K, THREADS > 1 ? SECCOMP_RET_ALLOW : SECCOMP_RET_TRAP),
      BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, __NR_exit, 0, 1),
      BPF_STMT(BPF_RET|BPF_K, THREADS > 1 ? SECCOMP_RET_ALLOW : SECCOMP_RET_TRAP),
      BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, __NR_futex, 0, 1),
      BPF_STMT(BPF_RET|BPF_K, THREADS > 1 ? SECCOMP_RET_ALLOW : SECCOMP_RET_TRAP),
      BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, __NR_get_robust_list, 0, 1),
      BPF_STMT(BPF_RET|BPF_K, THREADS > 1 ? SECCOMP_RET_ALLOW : SECCOMP_RET_TRAP),
      BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, __NR_madvise, 0, 1),
      BPF_STMT(BPF_RET|BPF_K, THREADS > 1 ? SECCOMP_RET_ALLOW : SECCOMP_RET_TRAP),
      BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, __NR_mprotect, 0, 1),
      BPF_STMT(BPF_RET|BPF_K, THREADS > 1 ? SECCOMP_RET_ALLOW : SECCOMP_RET_TRAP),
      // XXX: it would be nice to avoid open() but pthreads seems to open libgcc.
      BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, __NR_open, 0, 1),
      BPF_STMT(BPF_RET|BPF_K, THREADS > 1 ? SECCOMP_RET_ALLOW : SECCOMP_RET_TRAP),
      BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, __NR_read, 0, 1),
      BPF_STMT(BPF_RET|BPF_K, THREADS > 1 ? SECCOMP_RET_ALLOW : SECCOMP_RET_TRAP),
      BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, __NR_set_robust_list, 0, 1),
      BPF_STMT(BPF_RET|BPF_K, THREADS > 1 ? SECCOMP_RET_ALLOW : SECCOMP_RET_TRAP),

      /* Deny everything else. On a disallowed syscall, we trap instead of
       * killing to allow the user to debug the failure. If you are debugging
       * seccomp denials, strace the checker and find the number of the denied
       * syscall in the first si_value parameter reported in the terminating
       * SIG_SYS.
       */
      BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),
    };

    static const struct sock_fprog filter_program = {
      .len = sizeof(filter) / sizeof(filter[0]),
      .filter = filter,
    };

    /* Apply the above filter to ourselves. */
    r = prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &filter_program, 0, 0);
    if (r != 0) {
      perror("prctl(PR_SET_SECCOMP) failed");
      exit(EXIT_FAILURE);
    }

    return;
  }
  #endif
#endif

  /* No sandbox available. */
  fprintf(stderr, "no sandboxing facilities available\n");
  exit(EXIT_FAILURE);
}

/******************************************************************************/

// ANSI colour code support.

static bool istty;

static const char *green() {
  if (COLOR == ON || (COLOR == AUTO && istty))
    return "\033[32m";
  return "";
}

static const char *red() {
  if (COLOR == ON || (COLOR == AUTO && istty))
    return "\033[31m";
  return "";
}

static const char *yellow() {
  if (COLOR == ON || (COLOR == AUTO && istty))
    return "\033[33m";
  return "";
}

static const char *bold() {
  if (COLOR == ON || (COLOR == AUTO && istty))
    return "\033[1m";
  return "";
}

static const char *reset() {
  if (COLOR == ON || (COLOR == AUTO && istty))
    return "\033[0m";
  return "";
}

/*******************************************************************************
 * MurmurHash by Austin Appleby                                                *
 *                                                                             *
 * More information on this at https://github.com/aappleby/smhasher/           *
 ******************************************************************************/

static uint64_t MurmurHash64A(const void *key, size_t len) {

  static const uint64_t seed = 0;

  static const uint64_t m = UINT64_C(0xc6a4a7935bd1e995);
  static const unsigned r = 47;

  uint64_t h = seed ^ (len * m);

  const unsigned char *data = key;
  const unsigned char *end = data + len / sizeof(uint64_t) * sizeof(uint64_t);

  while (data != end) {

    uint64_t k;
    memcpy(&k, data, sizeof(k));
    data += sizeof(k);

    k *= m;
    k ^= k >> r;
    k *= m;

    h ^= k;
    h *= m;
  }

  const unsigned char *data2 = data;

  switch (len & 7) {
    case 7: h ^= (uint64_t)data2[6] << 48;
    case 6: h ^= (uint64_t)data2[5] << 40;
    case 5: h ^= (uint64_t)data2[4] << 32;
    case 4: h ^= (uint64_t)data2[3] << 24;
    case 3: h ^= (uint64_t)data2[2] << 16;
    case 2: h ^= (uint64_t)data2[1] << 8;
    case 1: h ^= (uint64_t)data2[0];
    h *= m;
  }

  h ^= h >> r;
  h *= m;
  h ^= h >> r;

  return h;
}

/******************************************************************************/

/* A lock that should be held whenever printing to stdout or stderr. This is a
 * way to prevent the output of one thread being interleaved with the output of
 * another.
 */
static pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

static void print_lock(void) {
  int r __attribute__((unused)) = pthread_mutex_lock(&print_mutex);
  assert(r == 0);
}

static void print_unlock(void) {
  int r __attribute__((unused)) = pthread_mutex_unlock(&print_mutex);
  assert(r == 0);
}

/* Supporting for tracing specific operations. This can be enabled during
 * checker generation with '--trace ...' and is useful for debugging Rumur
 * itself.
 */
static __attribute__((format(printf, 2, 3))) void trace(
  enum trace_category_t category, const char *fmt, ...) {

  if (category & TRACES_ENABLED) {
    va_list ap;
    va_start(ap, fmt);

    print_lock();

    (void)fprintf(stderr, "%sTRACE%s:", yellow(), reset());
    (void)vfprintf(stderr, fmt, ap);
    (void)fprintf(stderr, "\n");

    print_unlock();
    va_end(ap);
  }
}

/* The state of the current model. */
struct state {
  const struct state *previous;

  uint8_t data[STATE_SIZE_BYTES];
};

/* Print a counterexample trace terminating at the given state. This function
 * assumes that the caller already holds print_mutex.
 */
static unsigned print_counterexample(const struct state *s);

/* "Exit" the current thread. This takes into account which thread we are. I.e.
 * the correct way to exit the checker is for every thread to eventually call
 * this function.
 */
static _Noreturn int exit_with(int status);

static __attribute__((format(printf, 3, 4))) _Noreturn void error(
  const struct state *s, bool retain, const char *fmt, ...) {

  unsigned long prior_errors = error_count++;

  if (prior_errors < MAX_ERRORS) {

    print_lock();

    if (s != NULL) {
      fprintf(stderr, "The following is the error trace for the error:\n\n");
    } else {
      fprintf(stderr, "Result:\n\n");
    }

    fprintf(stderr, "\t%s%s", red(), bold());
    va_list ap;
    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    fprintf(stderr, "%s\n\n", reset());
    va_end(ap);

    if (s != NULL) {
      print_counterexample(s);
      fprintf(stderr, "End of the error trace.\n\n");
    }

    print_unlock();
  }

  if (!retain) {
    free((void*)s);
  }

#ifdef __clang__
  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Wtautological-compare"
#elif defined(__GNUC__)
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wtype-limits"
#endif
  if (prior_errors < MAX_ERRORS - 1) {
#ifdef __clang__
  #pragma clang diagnostic pop
#elif defined(__GNUC__)
  #pragma GCC diagnostic pop
#endif
    assert(JMP_BUF_NEEDED && "longjmping without a setup jmp_buf");
    longjmp(checkpoint, 1);
  }

  exit_with(EXIT_FAILURE);
}

/* Signal an out-of-memory condition and terminate abruptly. */
static _Noreturn void oom(void) {
  fputs("out of memory", stderr);
  exit(EXIT_FAILURE);
}

static void *xmalloc(size_t size) {
  void *p = malloc(size);
  if (p == NULL) {
    oom();
  }
  return p;
}

static void *xcalloc(size_t count, size_t size) {
  void *p = calloc(count, size);
  if (p == NULL) {
    oom();
  }
  return p;
}

static struct state *state_new(void) {
  return xcalloc(1, sizeof(struct state));
}

static int state_cmp(const struct state *a, const struct state *b) {
  return memcmp(a->data, b->data, sizeof(a->data));
}

static bool state_eq(const struct state *a, const struct state *b) {
  return state_cmp(a, b) == 0;
}

static struct state *state_dup(const struct state *s) {
  struct state *n = xmalloc(sizeof(*n));
  memcpy(n->data, s->data, sizeof(n->data));
  n->previous = s;
  return n;
}

static size_t state_hash(const struct state *s) {
  return (size_t)MurmurHash64A(s->data, sizeof(s->data));
}

/* Print a state to stderr. This function is generated. This function assumes
 * that the caller already holds print_mutex.
 */
static void state_print(const struct state *s);

static unsigned print_counterexample(const struct state *s) {

  if (s == NULL) {
    return 0;
  }

  /* Recurse so that we print the states in reverse-linked order, which
   * corresponds to the order in which they were traversed.
   */
  unsigned step = print_counterexample(s->previous) + 1;

  state_print(s);
  fprintf(stderr, "----------\n\n");
  return step;
}

struct handle {
  uint8_t *base;
  size_t offset;
  size_t width;
};

static __attribute__((unused)) bool handle_aligned(struct handle h) {
  return h.offset % 8 == 0 && h.width % 8 == 0;
}

static struct handle handle_align(struct handle h) {

  size_t offset = h.offset - (h.offset % 8);
  size_t width = h.width + (h.offset % 8);
  if (width % 8 != 0) {
    width += 8 - width % 8;
  }

  return (struct handle){
    .base = h.base,
    .offset = offset,
    .width = width,
  };
}

struct handle state_handle(const struct state *s, size_t offset, size_t width) {

  assert(sizeof(s->data) * CHAR_BIT - width >= offset && "generating an out of "
    "bounds handle in state_handle()");

  return (struct handle){
    .base = (uint8_t*)s->data,
    .offset = offset,
    .width = width,
  };
}

static unsigned __int128 handle_extract(struct handle h) {

  ASSERT(handle_aligned(h) && "extraction of unaligned handle");

  unsigned __int128 v = 0;
  for (size_t i = 0; i < h.width / 8; i++) {
    unsigned __int128 byte = ((unsigned __int128)*(h.base + h.offset / 8 + i)) << (i * 8);
    v |= byte;
  }

  return v;
}

static void handle_insert(struct handle h, unsigned __int128 v) {

  ASSERT(handle_aligned(h) && "insertion to unaligned handle");

  for (size_t i = 0; i < h.width / 8; i++) {
    *(h.base + h.offset / 8 + i) = (uint8_t)(v >> (i * 8));
  }
}

static value_t handle_read_raw(struct handle h) {
  static_assert(sizeof(unsigned __int128) > sizeof(value_t),
    "handle_read_raw() is implemented by reading data into a 128-bit scalar, "
    "potentially reading more than the width of a value. Value type is larger "
    "than 128 bits which prevents this.");

  if (h.width == 0) {
    trace(TC_HANDLE_READS, "read value %" PRIVAL " from handle { %p, %zu, %zu }",
      (value_t)0, h.base, h.offset, h.width);
    return 0;
  }

  struct handle aligned = handle_align(h);
  unsigned __int128 v = handle_extract(aligned);
  v >>= h.offset % 8;
  v &= (((unsigned __int128)1) << h.width) - 1;

  value_t dest = (value_t)v;

  trace(TC_HANDLE_READS, "read value %" PRIVAL " from handle { %p, %zu, %zu }",
    dest, h.base, h.offset, h.width);

  return dest;
}

static value_t decode_value(value_t lb, value_t ub, value_t v) {

  value_t dest = v;

  bool r __attribute__((unused)) = __builtin_sub_overflow(dest, 1, &dest) ||
    __builtin_add_overflow(dest, lb, &dest) || dest < lb || dest > ub;

  ASSERT(!r && "read of out-of-range value");

  return dest;
}

static __attribute__((unused)) value_t handle_read(const struct state *s,
    value_t lb, value_t ub, struct handle h) {

  /* If we happen to be reading from the current state, do a sanity check that
   * we're only reading within bounds.
   */
  assert((h.base != (uint8_t*)s->data /* not a read from the current state */
    || sizeof(s->data) * CHAR_BIT - h.width >= h.offset) /* in bounds */
    && "out of bounds read in handle_read()");

  value_t dest = handle_read_raw(h);

  if (dest == 0) {
    error(s, false, "read of undefined value");
  }

  return decode_value(lb, ub, dest);
}

static void handle_write_raw(struct handle h, value_t value) {

  trace(TC_HANDLE_WRITES, "writing value %" PRIVAL " to handle { %p, %zu, %zu }",
    value, h.base, h.offset, h.width);

  if (h.width == 0) {
    return;
  }

  /* Extract the byte-aligned region in which we need to write this value. */
  struct handle aligned = handle_align(h);
  unsigned __int128 v = handle_extract(aligned);

  size_t bit_offset = h.offset % 8;
  static const unsigned __int128 one = 1;
  unsigned __int128 low_mask = bit_offset == 0 ? 0 : (one << bit_offset) - 1;
  unsigned __int128 value_mask = ((one << (bit_offset + h.width)) - 1) & ~low_mask;

  /* Write new value into the relevant slice in the middle of the extracted
   * data.
   */
  v = (v & ~value_mask) | (((unsigned __int128)value) << bit_offset);

  /* Write back this data into the target location. */
  handle_insert(aligned, v);
}

static __attribute__((unused)) void handle_write(const struct state *s,
    value_t lb, value_t ub, struct handle h, value_t value) {

  static_assert(sizeof(unsigned __int128) > sizeof(value_t),
    "handle_write() is implemented by reading data into a 128-bit scalar and "
    "then operating on it using 128-bit operations. Value type is larger than "
    "128 bits which prevents this.");

  /* If we happen to be writing to the current state, do a sanity check that
   * we're only writing within bounds.
   */
  assert((h.base != (uint8_t*)s->data /* not a write to the current state */
    || sizeof(s->data) * CHAR_BIT - h.width >= h.offset) /* in bounds */
    && "out of bounds write in handle_write()");

  if (value < lb || value > ub || __builtin_sub_overflow(value, lb, &value) ||
      __builtin_add_overflow(value, 1, &value)) {
    error(s, false, "write of out-of-range value");
  }

  handle_write_raw(h, value);
}

static __attribute__((unused)) void handle_zero(struct handle h) {

  uint8_t *p = h.base + h.offset / 8;

  /* Zero out up to a byte-aligned offset. */
  if (h.offset % 8 != 0) {
    uint8_t mask = (UINT8_C(1) << (h.offset % 8)) - 1;
    if (h.width < 8 - h.offset % 8) {
      mask |= UINT8_MAX & ~((UINT8_C(1) << (h.offset % 8 + h.width)) - 1);
    }
    *p &= mask;
    p++;
    if (h.width < 8 - h.offset % 8) {
      return;
    }
    h.width -= 8 - h.offset % 8;
  }

  /* Zero out as many bytes as we can. */
  memset(p, 0, h.width / 8);
  p += h.width / 8;
  h.width -= h.width / 8 * 8;

  /* Zero out the trailing bits in the final byte. */
  if (h.width > 0) {
    uint8_t mask = ~((UINT8_C(1) << h.width) - 1);
    *p &= mask;
  }
}

static __attribute__((unused)) void handle_copy(struct handle a,
    struct handle b) {

  ASSERT(a.width == b.width && "copying between handles of different sizes");

  uint8_t *dst = a.base + a.offset / 8;
  size_t dst_off = a.offset % 8;
  size_t width = a.width;
  const uint8_t *src = b.base + b.offset / 8;
  size_t src_off = b.offset % 8;

  /* FIXME: This does a bit-by-bit copy which almost certainly could be
   * accelerated by detecting byte-boundaries and complementary alignment and
   * then calling memcpy when possible.
   */

  for (size_t i = 0; i < a.width; i++) {

    uint8_t *dst = a.base + (a.offset + i) / 8;
    size_t dst_off = (a.offset + i) % 8;

    const uint8_t *src = b.base + (b.offset + i) / 8;
    size_t src_off = (b.offset + i) % 8;

    uint8_t or_mask = ((*src >> src_off) & UINT8_C(1)) << dst_off;
    uint8_t and_mask = ~(UINT8_C(1) << dst_off);

    *dst = (*dst & and_mask) | or_mask;
  }
}

static __attribute__((unused)) struct handle handle_narrow(struct handle h,
  size_t offset, size_t width) {

  ASSERT(h.offset + offset + width <= h.offset + h.width &&
    "narrowing a handle with values that actually expand it");

  size_t r __attribute__((unused));
  assert(!__builtin_add_overflow(h.offset, offset, &r) &&
    "narrowing handle overflows a size_t");

  return (struct handle){
    .base = h.base,
    .offset = h.offset + offset,
    .width = width,
  };
}

static __attribute__((unused)) struct handle handle_index(const struct state *s,
  size_t element_width, value_t index_min, value_t index_max,
  struct handle root, value_t index) {

  if (index < index_min || index > index_max) {
    error(s, false, "index out of range");
  }

  size_t r1, r2;
  if (__builtin_sub_overflow(index, index_min, &r1) ||
      __builtin_mul_overflow(r1, element_width, &r2)) {
    error(s, false, "overflow when indexing array");
  }

  size_t r __attribute__((unused));
  assert(!__builtin_add_overflow(root.offset, r2, &r) &&
    "indexing handle overflows a size_t");

  return (struct handle){
    .base = root.base,
    .offset = root.offset + r2,
    .width = element_width,
  };
}

/* Overflow-safe helpers for doing bounded arithmetic. The compiler built-ins
 * used are implemented in modern GCC and Clang. If you're using another
 * compiler, you'll have to implement these yourself.
 */

static __attribute__((unused)) value_t add(const struct state *s, value_t a,
    value_t b) {

  value_t r;
  if (__builtin_add_overflow(a, b, &r)) {
    error(s, false, "integer overflow in addition");
  }
  return r;
}

static __attribute__((unused)) value_t sub(const struct state *s, value_t a,
    value_t b) {

  value_t r;
  if (__builtin_sub_overflow(a, b, &r)) {
    error(s, false, "integer overflow in subtraction");
  }
  return r;
}

static __attribute__((unused)) value_t mul(const struct state *s, value_t a,
    value_t b) {

  value_t r;
  if (__builtin_mul_overflow(a, b, &r)) {
    error(s, false, "integer overflow in multiplication");
  }
  return r;
}

static __attribute__((unused)) value_t divide(const struct state *s, value_t a,
    value_t b) {

  if (b == 0) {
    error(s, false, "division by zero");
  }

  if (a == VALUE_MIN && b == -1) {
    error(s, false, "integer overflow in division");
  }

  return a / b;
}

static __attribute__((unused)) value_t mod(const struct state *s, value_t a,
    value_t b) {

  if (b == 0) {
    error(s, false, "modulus by zero");
  }

  // Is INT64_MIN % -1 UD? Reading the C spec I'm not sure.
  if (a == VALUE_MIN && b == -1) {
    error(s, false, "integer overflow in modulo");
  }

  return a % b;
}

static __attribute__((unused)) value_t negate(const struct state *s,
    value_t a) {

  if (a == VALUE_MIN) {
    error(s, false, "integer overflow in negation");
  }

  return -a;
}

/* A version of quicksort that operates on "schedules," arrays of indices that
 * serve as a proxy for the collection being sorted.
 */
static __attribute__((unused)) void sort(
  int (*compare)(const struct state *s, size_t a, size_t b),
  size_t *schedule, struct state *s, size_t lower, size_t upper) {

  /* If we have nothing to sort, bail out. */
  if (lower >= upper) {
    return;
  }

  /* Use Hoare's partitioning algorithm to apply quicksort. */
  size_t i = lower - 1;
  size_t j = upper + 1;

  for (;;) {

    do {
      i++;
      assert(i >= lower && i <= upper && "out of bounds access in sort()");
    } while (compare(s, schedule[i], schedule[lower]) < 0);

    do {
      j--;
      assert(j >= lower && j <= upper && "out of bounds access in sort()");
    } while (compare(s, schedule[j], schedule[lower]) > 0);

    if (i >= j) {
      break;
    }

    /* Swap elements i and j. */
    size_t temp = schedule[i];
    schedule[i] = schedule[j];
    schedule[j] = temp;
  }

  sort(compare, schedule, s, lower, j);
  sort(compare, schedule, s, j + 1, upper);
}

/*******************************************************************************
 * State queue                                                                 *
 *                                                                             *
 * The following implements a per-thread queue for pending states. The only    *
 * supported operations are enqueueing and dequeueing states. A property we    *
 * maintain is that all states within all queues pass the current model's      *
 * invariants.                                                                 *
 ******************************************************************************/

struct queue_node {
  struct state *s;
  struct queue_node *next;
};

static struct {
  pthread_mutex_t lock;
  struct queue_node *head;
  size_t count;
} q[THREADS];

static void queue_init(void) {
  for (size_t i = 0; i < sizeof(q) / sizeof(q[0]); i++) {
    int r = pthread_mutex_init(&q[i].lock, NULL);
    if (r < 0) {
      fprintf(stderr, "pthread_mutex_init failed: %s\n", strerror(r));
      exit(EXIT_FAILURE);
    }
  }
}

size_t queue_enqueue(struct state *s, size_t queue_id) {
  assert(queue_id < sizeof(q) / sizeof(q[0]) && "out of bounds queue access");

  struct queue_node *n = xmalloc(sizeof(*n));
  n->s = s;

  int r __attribute__((unused)) = pthread_mutex_lock(&q[queue_id].lock);
  ASSERT(r == 0);

  n->next = q[queue_id].head;
  q[queue_id].head = n;
  q[queue_id].count++;

  trace(TC_QUEUE, "enqueued state %p into queue %zu, queue length is now %zu",
    s, queue_id, q[queue_id].count);

  size_t count = q[queue_id].count;

  r = pthread_mutex_unlock(&q[queue_id].lock);
  ASSERT(r == 0);

  return count;
}

const struct state *queue_dequeue(size_t *queue_id) {
  assert(queue_id != NULL && *queue_id < sizeof(q) / sizeof(q[0]) &&
    "out of bounds queue access");

  const struct state *s = NULL;

  for (size_t attempts = 0; attempts < sizeof(q) / sizeof(q[0]); attempts++) {

    int r __attribute__((unused)) = pthread_mutex_lock(&q[*queue_id].lock);
    ASSERT(r == 0);

    struct queue_node *n = q[*queue_id].head;
    if (n != NULL) {
      q[*queue_id].head = n->next;
      q[*queue_id].count--;
      trace(TC_QUEUE, "dequeued state %p from queue %zu, queue length is now "
        "%zu", n->s, *queue_id, q[*queue_id].count);
    }

    r = pthread_mutex_unlock(&q[*queue_id].lock);
    ASSERT(r == 0);

    if (n != NULL) {
      s = n->s;
      free(n);
      break;
    }

    /* Move to the next queue to try. */
    *queue_id = (*queue_id + 1) % (sizeof(q) / sizeof(q[0]));
  }

  return s;
}

/******************************************************************************/

/*******************************************************************************
 * Reference counted pointers                                                  *
 *                                                                             *
 * These are capable of encapsulating any generic pointer (void*). Note that   *
 * we rely on the existence of double-word atomics. On x86-64, you need to     *
 * use compiler flag '-mcx16' to get an efficient 128-bit cmpxchg.             *
 *                                                                             *
 * Of these functions, only the following are thread safe:                     *
 *                                                                             *
 *   * refcounted_ptr_get                                                      *
 *   * refcounted_ptr_put                                                      *
 *                                                                             *
 * The caller is expected to coordinate with other threads to exclude them     *
 * operating on the relevant refcounted_ptr_t when using one of the other      *
 * functions:                                                                  *
 *                                                                             *
 *   * refcounted_ptr_set                                                      *
 *   * refcounted_ptr_shift                                                    *
 *                                                                             *
 ******************************************************************************/

struct refcounted_ptr {
  void *ptr;
  size_t count;
};

#if __SIZEOF_POINTER__ <= 4
  typedef uint64_t refcounted_ptr_t;
#elif __SIZEOF_POINTER__ <= 8
  typedef unsigned __int128 refcounted_ptr_t;
#else
  #error "unexpected pointer size; what scalar type to use for refcounted_ptr_t?"
#endif

static_assert(sizeof(struct refcounted_ptr) <= sizeof(refcounted_ptr_t),
  "refcounted_ptr does not fit in a refcounted_ptr_t, which we need to operate "
  "on it atomically");

static void refcounted_ptr_set(refcounted_ptr_t *p, void *ptr) {

  /* Read the current state of the pointer. Note, we don't bother doing this
   * atomically as it's only for debugging and no one else should be using the
   * pointer source right now.
   */
  struct refcounted_ptr p2;
  memcpy(&p2, p, sizeof(*p));
  ASSERT(p2.count == 0 && "overwriting a pointer source while someone still "
    "has a reference to this pointer");

  /* Set the current source pointer with no outstanding references. */
  p2.ptr = ptr;
  p2.count = 0;

  /* Commit the result. Again, we do not operate atomically because no one else
   * should be using the pointer source.
   */
  memcpy(p, &p2, sizeof(*p));
}

static void *refcounted_ptr_get(refcounted_ptr_t *p) {

  refcounted_ptr_t old, new;
  void *ret;
  bool r;

  do {

    /* Read the current state of the pointer. */
#ifdef __x86_64__
    /* It seems MOV on x86-64 is not guaranteed to be atomic on 128-bit
     * naturally aligned memory. The way to work around this is apparently the
     * following degenerate CMPXCHG.
     */
    old = __sync_val_compare_and_swap(p, *p, *p);
#else
    old = __atomic_load_n(p, __ATOMIC_SEQ_CST);
#endif
    struct refcounted_ptr p2;
    memcpy(&p2, &old, sizeof(old));

    /* Take a reference to it. */
    p2.count++;
    ret = p2.ptr;

    /* Try to commit our results. */
    memcpy(&new, &p2, sizeof(new));
#ifdef __x86_64__
    /* Make GCC >= 7.1 emit cmpxchg on x86-64. See
     * https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80878.
     */
    r = __sync_bool_compare_and_swap(p, old, new);
#else
    r = __atomic_compare_exchange_n(p, &old, new, false, __ATOMIC_SEQ_CST,
      __ATOMIC_SEQ_CST);
#endif
  } while (!r);

  return ret;
}

static size_t refcounted_ptr_put(refcounted_ptr_t *p,
  void *ptr __attribute__((unused))) {

  refcounted_ptr_t old, new;
  size_t ret;
  bool r;

  do {

    /* Read the current state of the pointer. */
#ifdef __x86_64__
    /* It seems MOV on x86-64 is not guaranteed to be atomic on 128-bit
     * naturally aligned memory. The way to work around this is apparently the
     * following degenerate CMPXCHG.
     */
    old = __sync_val_compare_and_swap(p, *p, *p);
#else
    old = __atomic_load_n(p, __ATOMIC_SEQ_CST);
#endif
    struct refcounted_ptr p2;
    memcpy(&p2, &old, sizeof(old));

    /* Release our reference to it. */
    ASSERT(p2.ptr == ptr && "releasing a reference to a pointer after someone "
      "has changed the pointer source");
    ASSERT(p2.count > 0 && "releasing a reference to a pointer when it had no "
      "outstanding references");
    p2.count--;
    ret = p2.count;

    /* Try to commit our results. */
    memcpy(&new, &p2, sizeof(new));
#ifdef __x86_64__
    /* Make GCC >= 7.1 emit cmpxchg on x86-64. See
     * https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80878.
     */
    r = __sync_bool_compare_and_swap(p, old, new);
#else
    r = __atomic_compare_exchange_n(p, &old, new, false, __ATOMIC_SEQ_CST,
      __ATOMIC_SEQ_CST);
#endif
  } while (!r);

  return ret;
}

static void refcounted_ptr_shift(refcounted_ptr_t *current, refcounted_ptr_t *next) {

  /* None of the operations in this function are performed atomically because we
   * assume the caller has synchronised with other threads via other means.
   */

  /* The pointer we're about to overwrite should not be referenced. */
  struct refcounted_ptr p __attribute__((unused));
  memcpy(&p, current, sizeof(*current));
  ASSERT(p.count == 0 && "overwriting a pointer that still has outstanding "
    "references");

  /* Shift the next value into the current pointer. */
  *current = *next;

  /* Blank the value we just shifted over. */
  *next = 0;
}

/******************************************************************************/

/*******************************************************************************
 * Thread rendezvous support                                                   *
 ******************************************************************************/

static pthread_mutex_t rendezvous_lock; /* mutual exclusion mechanism for below. */
static pthread_cond_t rendezvous_cond;  /* sleep mechanism for below. */
static size_t running_count = 1;            /* how many threads are opted in to rendezvous? */
static size_t rendezvous_pending = 1;   /* how many threads are opted in and not sleeping? */

static void rendezvous_init(void) {
  int r = pthread_mutex_init(&rendezvous_lock, NULL);
  if (r != 0) {
    fprintf(stderr, "pthread_mutex_init failed: %s\n", strerror(r));
    exit(EXIT_FAILURE);
  }

  r = pthread_cond_init(&rendezvous_cond, NULL);
  if (r != 0) {
    fprintf(stderr, "pthread_cond_init failed: %s\n", strerror(r));
    exit(EXIT_FAILURE);
  }
}

/* Call this at the start of a rendezvous point.
 *
 * This is a low level function, not expected to be directly used outside of the
 * context of the rendezvous implementation.
 *
 * @return True if the caller was the last to arrive and henceforth dubbed the
 *   'leader'.
 */
static bool rendezvous_arrive(void) {
  int r __attribute__((unused)) = pthread_mutex_lock(&rendezvous_lock);
  assert(r == 0);

  /* Take a token from the rendezvous down-counter. */
  assert(rendezvous_pending > 0);
  rendezvous_pending--;

  /* If we were the last to arrive then it was our arrival that dropped the
   * counter to zero.
   */
  return rendezvous_pending == 0;
}

/* Call this at the end of a rendezvous point.
 *
 * This is a low level function, not expected to be directly used outside of the
 * context of the rendezvous implementation.
 *
 * @param leader Whether the caller is the 'leader'. If you call this when you
 *   are the 'leader' it will unblock all 'followers' at the rendezvous point.
 */
static void rendezvous_depart(bool leader) {
  int r __attribute((unused));

  if (leader) {
    /* Reset the counter for the next rendezvous. */
    assert(rendezvous_pending == 0 && "a rendezvous point is being exited "
      "while some participating threads have yet to arrive");
    rendezvous_pending = running_count;

    /* Wake up the 'followers'. */
    r = pthread_cond_broadcast(&rendezvous_cond);
    assert(r == 0);

  } else {

    /* Wait on the 'leader' to wake us up. */
    r = pthread_cond_wait(&rendezvous_cond, &rendezvous_lock);
    assert(r == 0);
  }

  r = pthread_mutex_unlock(&rendezvous_lock);
  assert(r == 0);
}

/* Exposed friendly function for performing a rendezvous. */
static void rendezvous(void) {
  bool leader = rendezvous_arrive();
  rendezvous_depart(leader);
}

/* Remove the caller from the pool of threads who participate in this
 * rendezvous.
 */
static void rendezvous_opt_out(void) {

retry:;

  /* "Arrive" at the rendezvous to decrement the count of outstanding threads.
   */
  bool leader = rendezvous_arrive();

  if (leader && running_count > 1) {
    /* We unfortunately opted out of this rendezvous while the remaining threads
     * were arriving at one and we were the last to arrive. Let's pretend we are
     * participating in the rendezvous and unblock them.
     */
    rendezvous_depart(true);

    /* Re-attempt opting-out. */
    goto retry;
  }

  /* Remove ourselves from the known threads. */
  assert(running_count > 0);
  running_count--;

  int r __attribute__((unused)) = pthread_mutex_unlock(&rendezvous_lock);
  assert(r == 0);
}

/******************************************************************************/

/*******************************************************************************
 * 'Slots', an opaque wrapper around a state pointer                           *
 *                                                                             *
 * See usage of this in the state set below for its purpose.                   *
 ******************************************************************************/

typedef uintptr_t slot_t;

static slot_t slot_empty(void) {
  return 0;
}

static bool slot_is_empty(slot_t s) {
  return s == slot_empty();
}

static bool slot_is_tombstone(slot_t s) {
  return (s & 0x1) == 0x1;
}

static slot_t slot_bury(slot_t s) {
  ASSERT(!slot_is_tombstone(s));
  return s | 0x1;
}

static struct state *slot_to_state(slot_t s) {
  ASSERT(!slot_is_empty(s));
  ASSERT(!slot_is_tombstone(s));
  return (struct state*)s;
}

static slot_t state_to_slot(const struct state *s) {
  return (slot_t)s;
}

/******************************************************************************/

/*******************************************************************************
 * State set                                                                   *
 *                                                                             *
 * The following implementation provides a set for storing the seen states.    *
 * There is no support for testing whether something is in the set or for      *
 * removing elements, only thread-safe insertion of elements.                  *
 ******************************************************************************/

enum { INITIAL_SET_SIZE_EXPONENT = sizeof(unsigned long long) * 8 - 1 -
  __builtin_clzll(SET_CAPACITY / sizeof(struct state*) / sizeof(struct state)) };

struct set {
  slot_t *bucket;
  size_t size_exponent;
  size_t count;
};

/* Some utility functions for dealing with exponents. */

static size_t set_size(const struct set *set) {
  return ((size_t)1) << set->size_exponent;
}

static size_t set_index(const struct set *set, size_t index) {
  return index & (set_size(set) - 1);
}

/* The states we have encountered. This collection will only ever grow while
 * checking the model. Note that we have a global reference-counted pointer and
 * a local bare pointer. See below for an explanation.
 */
static refcounted_ptr_t global_seen;
static _Thread_local struct set *local_seen;

/* The "next" 'global_seen' value. See below for an explanation. */
static refcounted_ptr_t next_global_seen;

/* Now the explanation I teased... When the set capacity exceeds a threshold
 * (see 'set_expand' related logic below) it is expanded and the reference
 * tracking within the 'refcounted_ptr_t's comes in to play. We need to allocate
 * a new seen set ('next_global_seen'), copy over all the elements (done in
 * 'set_migrate'), and then "shift" the new set to become the current set. The
 * role of the reference counts of both 'global_seen' and 'next_global_seen' in
 * all of this is to detect when the last thread releases its reference to the
 * old seen set and hence can deallocate it.
 */

/* The next chunk to migrate from the old set to the new set. What exactly a
 * "chunk" is is covered in 'set_migrate'.
 */
static size_t next_migration;

/* A mechanism for synchronisation in 'set_expand'. */
static pthread_mutex_t set_expand_mutex;

static void set_expand_lock(void) {
  int r __attribute__((unused)) = pthread_mutex_lock(&set_expand_mutex);
  ASSERT(r == 0);
}

static void set_expand_unlock(void) {
  int r __attribute__((unused)) = pthread_mutex_unlock(&set_expand_mutex);
  ASSERT(r == 0);
}


static void set_init(void) {

  int r = pthread_mutex_init(&set_expand_mutex, NULL);
  if (r < 0) {
    fprintf(stderr, "pthread_mutex_init failed: %s\n", strerror(r));
    exit(EXIT_FAILURE);
  }

  /* Allocate the set we'll store seen states in at some conservative initial
   * size.
   */
  struct set *set = xmalloc(sizeof(*set));
  set->size_exponent = INITIAL_SET_SIZE_EXPONENT;
  set->bucket = xcalloc(set_size(set), sizeof(set->bucket[0]));
  set->count = 0;

  /* Stash this somewhere for threads to later retrieve it from. Note that we
   * initialize its reference count to zero as we (the setup logic) are not
   * using it beyond this function.
   */
  refcounted_ptr_set(&global_seen, set);
}

static void set_thread_init(void) {
  /* Take a local reference to the global seen set. */
  local_seen = refcounted_ptr_get(&global_seen);
}

static void set_migrate(void) {

  trace(TC_SET, "assisting in set migration...");

  /* Size of a migration chunk. Threads in this function grab a chunk at a time
   * to migrate.
   */
  enum { CHUNK_SIZE = 4096 / sizeof(local_seen->bucket[0]) /* slots */ };

  /* Take a pointer to the target set for the migration. */
  struct set *next = refcounted_ptr_get(&next_global_seen);

  for (;;) {

    size_t chunk = __atomic_fetch_add(&next_migration, 1, __ATOMIC_SEQ_CST);
    size_t start = chunk * CHUNK_SIZE;
    size_t end = start + CHUNK_SIZE;

    /* Bail out if we've finished migrating all of the set. */
    if (start >= set_size(local_seen)) {
      break;
    }

    // TODO: The following algorithm assumes insertions can collide. That is, it
    // operates atomically on slots because another thread could be migrating
    // and also targeting the same slot. If we were to more closely wick to the
    // Maier design this would not be required.

    for (size_t i = start; i < end; i++) {
retry:;

      /* Retrieve the slot element and try to mark it as migrated. */
      slot_t s = __atomic_load_n(&local_seen->bucket[i], __ATOMIC_SEQ_CST);
      ASSERT(!slot_is_tombstone(s) && "attempted double slot migration");
      if (!__atomic_compare_exchange_n(&local_seen->bucket[i], &s, slot_bury(s),
          false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        goto retry;
      }

      /* If the current slot contained a state, rehash it and insert it into the
       * new set. Note we don't need to do any state comparisons because we know
       * everything in the old state is unique.
       */
      if (!slot_is_empty(s)) {
        size_t index = set_index(next, state_hash(slot_to_state(s)));
        for (size_t j = index; ; j = set_index(next, j + 1)) {
          slot_t expected = slot_empty();
          if (__atomic_compare_exchange_n(&next->bucket[j], &expected, s, false,
              __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            /* Found an empty slot and inserted successfully. */
            break;
          }
        }
      }
    }

  }

  /* Release our reference to the old set now we're done with it. */
  size_t count = refcounted_ptr_put(&global_seen, local_seen);

  if (count == 0) {

    trace(TC_SET, "arrived at rendezvous point as leader");

    /* At this point, we know no one is still updating the old set's count, so
     * we can migrate its value to the next set.
     */
    next->count = local_seen->count;

    /* We were the last thread to release our reference to the old set. Clean it
     * up now. Note that we're using the pointer we just gave up our reference
     * count to, but we know no one else will be invalidating it.
     */
    free(local_seen->bucket);
    free(local_seen);

    /* Update the global pointer to the new set. We know all the above
     * migrations have completed and no one needs the old set.
     */
    refcounted_ptr_shift(&global_seen, &next_global_seen);
  }

  /* Now we need to make sure all the threads get to this point before any one
   * thread leaves. The purpose of this is to guarantee we only ever have at
   * most two seen sets "in flight". Without this rendezvous, one thread could
   * race ahead, fill the new set, and then decide to expand again while some
   * are still working on the old set. It's possible to make such a scheme work
   * but the synchronisation requirements just seem too complicated.
   */
  rendezvous();

  /* We're now ready to resume model checking. Note that we already have a
   * (reference counted) pointer to the now-current global seen set, so we don't
   * need to take a fresh reference to it.
   */
  local_seen = next;
}

static void set_expand(void) {

  set_expand_lock();

  /* Check if another thread beat us to expanding the set. */
  struct set *s = refcounted_ptr_get(&next_global_seen);
  (void)refcounted_ptr_put(&next_global_seen, s);
  if (s != NULL) {
    /* Someone else already expanded it. Join them in the migration effort. */
    set_expand_unlock();
    trace(TC_SET, "attempted expansion failed because another thread got there "
      "first");
    set_migrate();
    return;
  }

  trace(TC_SET, "expanding set from %zu slots to %zu slots...",
    (((size_t)1) << local_seen->size_exponent) / sizeof(slot_t),
    (((size_t)1) << (local_seen->size_exponent + 1)) / sizeof(slot_t));

  /* Create a set of double the size. */
  struct set *set = xmalloc(sizeof(*set));
  set->size_exponent = local_seen->size_exponent + 1;
  set->bucket = xcalloc(set_size(set), sizeof(set->bucket[0]));
  set->count = 0; /* will be updated in set_migrate(). */

  /* Advertise this as the newly expanded global set. */
  refcounted_ptr_set(&next_global_seen, set);

  /* We now need to migrate all slots from the old set to the new one, but we
   * can do this multithreaded.
   */
  next_migration = 0; /* initialise migration state */
  set_expand_unlock();
  set_migrate();
}

static bool set_insert(struct state *s, size_t *count) {

restart:;

  if (local_seen->count * 100 / set_size(local_seen) >= SET_EXPAND_THRESHOLD)
    set_expand();

  size_t index = set_index(local_seen, state_hash(s));

  size_t attempts = 0;
  for (size_t i = index; attempts < set_size(local_seen); i = set_index(local_seen, i + 1)) {

    /* Guess that the current slot is empty and try to insert here. */
    slot_t c = slot_empty();
    if (__atomic_compare_exchange_n(&local_seen->bucket[i], &c,
        state_to_slot(s), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
      /* Success */
      *count = __atomic_add_fetch(&local_seen->count, 1, __ATOMIC_SEQ_CST);
      trace(TC_SET, "added state %p, set size is now %zu", s, *count);

      /* The maximum possible size of the seen state set should be constrained
       * by the number of possible states based on how many bits we are using to
       * represent the state data.
       */
      if (STATE_SIZE_BITS < sizeof(size_t) * CHAR_BIT) {
        assert(*count <= SIZE_C(1) << STATE_SIZE_BITS && "seen set size "
          "exceeds total possible number of states");
      }

      return true;
    }

    if (slot_is_tombstone(c)) {
      /* This slot has been migrated. We need to rendezvous with other migrating
       * threads and restart our insertion attempt on the newly expanded set.
       */
      set_migrate();
      goto restart;
    }

    /* If we find this already in the set, we're done. */
    if (state_eq(s, slot_to_state(c))) {
      trace(TC_SET, "skipped adding state %p that was already in set", s);
      return false;
    }

    attempts++;
  }

  /* If we reach here, the set is full. Expand it and retry the insertion. */
  set_expand();
  return set_insert(s, count);
}

/******************************************************************************/

static time_t START_TIME;

static unsigned long long gettime() {
  return (unsigned long long)(time(NULL) - START_TIME);
}

/* Prototypes for generated functions. */
static void init(void);
static _Noreturn void explore(void);

static int exit_with(int status) {

  /* Opt out of the thread-wide rendezvous protocol. */
  rendezvous_opt_out();

  /* Make fired rule count visible globally. */
  rules_fired[thread_id] = rules_fired_local;

  if (thread_id == 0) {
    /* We are the initial thread. Wait on the others before exiting. */
#ifdef __clang__
  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Wtautological-compare"
#elif defined(__GNUC__)
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wtype-limits"
#endif
    for (size_t i = 0; phase == RUN &&
         i < sizeof(threads) / sizeof(threads[0]); i++) {
#ifdef __clang__
  #pragma clang diagnostic pop
#elif defined(__GNUC__)
  #pragma GCC diagnostic pop
#endif
      void *ret;
      int r = pthread_join(threads[i], &ret);
      if (r != 0) {
        print_lock();
        fprintf(stderr, "failed to join thread: %s\n", strerror(r));
        print_unlock();
        continue;
      }
      status |= (int)(intptr_t)ret;
    }

    /* We're now single-threaded again. */

    printf("\n"
           "==========================================================================\n"
           "\n"
           "Status:\n"
           "\n");
    if (error_count == 0) {
      printf("\t%s%sNo error found.%s\n", green(), bold(), reset());
    } else {
      printf("\t%s%s%lu error(s) found.%s\n", red(), bold(), error_count, reset());
    }
    printf("\n");

    /* Calculate the total number of rules fired. */
    uintmax_t fire_count = 0;
    for (size_t i = 0; i < sizeof(rules_fired) / sizeof(rules_fired[0]); i++) {
      fire_count += rules_fired[i];
    }

    /* Paranoid check that we didn't miscount during set insertions/expansions.
     */
    size_t count = 0;
    for (size_t i = 0; i < set_size(local_seen); i++) {
      if (!slot_is_empty(local_seen->bucket[i])) {
        count++;
      }
    }
    assert(count == local_seen->count && "seen set count is inconsistent at "
      "exit");

    printf("State Space Explored:\n"
           "\n"
           "\t%zu states, %" PRIuMAX " rules fired in %llus.\n",
           local_seen->count, fire_count, gettime());

    exit(status);
  } else {
    pthread_exit((void*)(intptr_t)status);
  }
}

static void *thread_main(void *arg) {

  /* Initialize (thread-local) thread identifier. */
  thread_id = (size_t)(uintptr_t)arg;

  set_thread_init();

  explore();
}

static void start_secondary_threads(void) {

  /* XXX: Kind of hacky. We've left the rendezvous down-counter at 1 until now
   * in case we triggered a rendezvous before starting the other threads (it
   * *can* happen). We bump it immediately -- i.e. before starting *any* of the
   * secondary threads -- because any of them could race us and trigger a
   * rendezvous before we exit the below loop and they need to know about the
   * thundering herd bearing down on them. It is safe to do this without holding
   * rendezvous_lock because we are still single threaded at this point.
   */
  assert(running_count == 1);
  running_count = THREADS;
  assert(rendezvous_pending == 1);
  rendezvous_pending = THREADS;

#ifdef __clang__
  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Wtautological-compare"
#elif defined(__GNUC__)
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wtype-limits"
#endif
  for (size_t i = 0; i < sizeof(threads) / sizeof(threads[0]); i++) {
#ifdef __clang__
  #pragma clang diagnostic pop
#elif defined(__GNUC__)
  #pragma GCC diagnostic pop
#endif
    int r = pthread_create(&threads[i], NULL, thread_main,
      (void*)(uintptr_t)(i + 1));
    if (r != 0) {
      fprintf(stderr, "pthread_create failed: %s\n", strerror(r));
      exit(EXIT_FAILURE);
    }
  }
}

int main(void) {

  sandbox();

  printf("Memory usage:\n"
         "\n"
         "\t* The size of each state is %zu bits (rounded up to %zu bytes).\n"
         "\t* The size of the hash table is %zu slots.\n"
         "\n",
         (size_t)STATE_SIZE_BITS, (size_t)STATE_SIZE_BYTES,
         ((size_t)1) << INITIAL_SET_SIZE_EXPONENT);

  START_TIME = time(NULL);

  if (COLOR == AUTO)
    istty = isatty(STDOUT_FILENO) != 0;

  rendezvous_init();

  set_init();
  queue_init();

  set_thread_init();

  init();

  printf("Progress Report:\n\n");

  explore();
}
