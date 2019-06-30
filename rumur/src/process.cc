#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include "process.h"
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

// compile this file with -DTEST_PROCESS to build a test harness
#ifdef TEST_PROCESS
  #define debug (&std::cerr)
#else
  #include "log.h"
#endif

#ifdef __APPLE__
    #include <crt_externs.h>
#endif

enum { READ_FD = 0, WRITE_FD = 1 };

// pipe through which we'll redirect SIGCHLD notifications
static int sigchld_pipe[2] = { -1, -1 };

// signal handler to redirect SIGCHLD into the above pipe
static void handler(int sigchld __attribute__((unused))) {

  assert(sigchld == SIGCHLD && "SIGCHLD handler received something other than "
    "SIGCHLD");

  assert(sigchld_pipe[WRITE_FD] != -1 && "SIGCHLD handler called before "
    "SIGCHLD pipe has been setup");

  (void)write(sigchld_pipe[WRITE_FD], "\0", 1);
}

static bool inited = false;

static int init(void) {

  if (pipe(sigchld_pipe) < 0) {
    *debug << "failed to create SIGCHLD pipe\n";
    goto fail;
  }

  // set the SIGCHLD pipe not to block on reading or writing
  {
    int r = sigchld_pipe[READ_FD];
    int w = sigchld_pipe[WRITE_FD];
    if (fcntl(r, F_SETFL, fcntl(r, F_GETFL) | O_NONBLOCK) == -1 ||
        fcntl(w, F_SETFL, fcntl(w, F_GETFL) | O_NONBLOCK) == -1) {
      *debug << "failed to set SIGCHLD pipe non-blocking\n";
      goto fail;
    }
  }

  // register our redirecting signal handler
  if (signal(SIGCHLD, handler) == SIG_ERR) {
    *debug << "failed to set SIGCHLD handler: " << strerror(errno) << "\n";
    goto fail;
  }

  inited = true;

  return 0;

fail:
  for (int &fd : sigchld_pipe) {
    if (fd != -1) {
      (void)close(fd);
      fd = -1;
    }
  }
  return -1;
}

static char **get_environ() {
#if __APPLE__
    /* Bizarrely Apple don't give programs a symbol for environ, but have an
     * indirect way of accessing it.
     */
    return *_NSGetEnviron();
#else
    /* some platforms fail to expose environ in a header (e.g. FreeBSD), so
     * declare it ourselves and assume it will be available when linking
     */
    extern char **environ;

    return environ;
#endif
}

static int max(int a, int b) {
  return a > b ? a : b;
}

int run(const std::vector<std::string> &args, const std::string &input,
  std::string &output) {

  if (!inited) {
    if (init() < 0)
      return -1;
  }

  // setup an argument vector
  std::vector<char*> argv;
  for (const std::string &a : args)
    argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);


  posix_spawn_file_actions_t fa;
  int in[2] = { -1, -1 };
  int out[2] = { -1, -1 };
  int ret = -1;
  std::ostringstream buffered_output;
  off_t input_offset = 0;


  if (posix_spawn_file_actions_init(&fa) < 0) {
    *debug << "failed file_actions_init: " << strerror(errno) << "\n";
    return -1;
  }

  // create some pipes we'll use to communicate with the child
  if (pipe(in) < 0 || pipe(out) < 0) {
    *debug<< "failed pipe: " << strerror(errno) << "\n";
    goto done;
  }

  // set the ends the parent (us) will use as non-blocking
  if (fcntl(in[WRITE_FD], F_SETFL, fcntl(in[WRITE_FD], F_GETFL) | O_NONBLOCK) == -1 ||
      fcntl(out[READ_FD], F_SETFL, fcntl(out[READ_FD], F_GETFL) | O_NONBLOCK) == -1) {
    *debug << "failed to set O_NONBLOCK: " << strerror(errno) << "\n";
    goto done;
  }

  // have the child close the ends it won't need
  if (posix_spawn_file_actions_addclose(&fa, in[WRITE_FD]) < 0 ||
      posix_spawn_file_actions_addclose(&fa, out[READ_FD]) < 0) {
    *debug << "failed file_actions_addclose: " << strerror(errno) << "\n";
    goto done;
  }

  // replace the child's stdin, stdout and stderr with the pipes
  if (posix_spawn_file_actions_adddup2(&fa, in[READ_FD], STDIN_FILENO) < 0 ||
      posix_spawn_file_actions_adddup2(&fa, out[WRITE_FD], STDOUT_FILENO) < 0 ||
      posix_spawn_file_actions_adddup2(&fa, out[WRITE_FD], STDERR_FILENO) < 0) {
    *debug << "failed file_actions_adddup2: " << strerror(errno) << "\n";
    goto done;
  }

  // spawn the child
  pid_t pid;
  if (posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), get_environ()) != 0) {
    *debug << "failed posix_spawnp: " << strerror(errno) << "\n";
    goto done;
  }

  // close the ends of the pipes we (the parent) don't need
  (void)close(in[READ_FD]);
  in[READ_FD] = -1;
  (void)close(out[WRITE_FD]);
  out[WRITE_FD] = -1;

  // now we're ready to sit in an event loop interacting with the child
  for (;;) {

    int nfds;

    // create a set of the out pipe and SIGCHLD to monitor for reading
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(out[READ_FD], &readfds);
    FD_SET(sigchld_pipe[READ_FD], &readfds);
    nfds = max(out[READ_FD], sigchld_pipe[READ_FD]);

    // create a set of only the in pipe (if still open) to monitor for writing
    fd_set writefds;
    FD_ZERO(&writefds);
    if (in[WRITE_FD] != -1) {
      FD_SET(in[WRITE_FD], &writefds);
      nfds = max(nfds, in[WRITE_FD]);
    }

    // wait for an event
    if (select(nfds, &readfds, &writefds, nullptr, nullptr) < 0) {
      // if our select call is correct, and any "error" should be an interrupt
      assert(errno == EAGAIN || errno == EINTR);
    }

    // clear any SIGCHLD notification
    if (FD_ISSET(sigchld_pipe[READ_FD], &readfds)) {
      char ignored[BUFSIZ];
      (void)read(sigchld_pipe[READ_FD], ignored, sizeof(ignored));
    }

    // read any data available from the child
    if (FD_ISSET(out[READ_FD], &readfds)) {
      ssize_t r;
      do {

        char buffer[BUFSIZ];
        do {
          r = read(out[READ_FD], buffer, sizeof(buffer));
        } while (r == -1 && errno == EINTR);

        if (r == -1) {
          if (errno == EAGAIN)
            break;
          *debug << "failed to read from child: " << strerror(errno) << "\n";
          goto done;
        }

        // retain anything we read
        for (size_t i = 0; i < (size_t)r; i++)
          buffered_output << buffer[i];

      } while (r > 0);
    }

    // write remaining data if the input pipe is ready
    if (in[WRITE_FD] != -1 && FD_ISSET(in[WRITE_FD], &writefds)) {
      if ((size_t)input_offset < input.size()) {

        ssize_t w;
        do {
          w = write(in[WRITE_FD], input.c_str() + input_offset,
            input.size() - input_offset);
        } while (w == -1 && errno == EINTR);

        if (w == -1 && errno != EAGAIN) {
          *debug << "failed to write to child: " << strerror(errno) << "\n";
          goto done;
        }

        if (w > 0) {
          input_offset += (off_t)w;
          if ((size_t)input_offset == input.size()) {
            // exhausted the input; send the child EOF
            (void)close(in[WRITE_FD]);
            in[WRITE_FD] = -1;
          }
        }
      }
    }

    // check if the child has exited
    int status;
    if (waitpid(pid, &status, WNOHANG) == pid) {
#if 0
      /* It's useful to uncomment this when testing new SMT modes or new Rumur
       * syntax, to make a solver failure crash Rumur. Obviously you don't want
       * this enabled in a production build because the solver is just an
       * arbitrary user-defined program that's free to fail or crash if it wants
       * to.
       */
      assert(WIFSTOPPED(status) ||
        (WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS));
#endif
      if (WIFEXITED(status)) {
        if (WEXITSTATUS(status) != EXIT_SUCCESS)
          *debug << "child returned exit status " << WEXITSTATUS(status) << "\n";
        break;
      }
      if (WIFSIGNALED(status)) {
        *debug << "child exited due to signal " << WTERMSIG(status) << "\n";
        break;
      }
    }
  }

  // success if we've reached here

  output = buffered_output.str();

  ret = 0;

done:

  for (int &fd : out) {
    if (fd != -1) {
      (void)close(fd);
      fd = -1;
    }
  }

  for (int &fd : in) {
    if (fd != -1) {
      (void)close(fd);
      fd = -1;
    }
  }

  (void)posix_spawn_file_actions_destroy(&fa);

  return ret;
}

static int __attribute__((unused)) test_process(int argc, char **argv) {

  if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0
      || strcmp(argv[1], "-?") == 0) {
    std::cerr
      << "Rumur Process.cc tester\n"
      << "\n"
      << " usage: " << argv[0] << " cmd args...\n";
    return EXIT_SUCCESS;
  }

  // construct argument list
  std::vector<std::string> args;
  for (size_t i = 1; i < (size_t)argc; i++) {
    assert(argv[i] != nullptr);
    args.emplace_back(argv[i]);
  }

  // read stdin until EOF
  std::ostringstream input;
  for (std::string line; std::getline(std::cin, line); ) {
    input << line << "\n";
  }

  // run our designated process
  std::string output;
  int r = run(args, input.str(), output);

  if (r == 0)
    std::cout << output;

  return r == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

#ifdef TEST_PROCESS
int main(int argc, char **argv) {
  return test_process(argc, argv);
}
#endif
