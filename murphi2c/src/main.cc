#include <cassert>
#include <cstdlib>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <rumur/rumur.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static std::string in_filename = "<stdin>";
static std::shared_ptr<std::istream> in;
static std::shared_ptr<std::ostream> out;

static void parse_args(int argc, char **argv) {

  for (;;) {
    static struct option options[] = {
      { "help", no_argument, 0, '?' },
      { "output", required_argument, 0, 'o' },
      { "version", no_argument, 0, 128 },
      { 0, 0, 0, 0 },
    };

    int option_index = 0;
    int c = getopt_long(argc, argv, "o:", options, &option_index);

    if (c == -1)
      break;

    switch (c) {

      case '?':
        std::cout << "TODO\n";
        exit(EXIT_SUCCESS);

      case 'o': {
        auto o = std::make_shared<std::ofstream>(optarg);
        if (!o->is_open()) {
          std::cerr << "failed to open " << optarg << "\n";
          exit(EXIT_FAILURE);
        }
        out = o;
        break;
      }

      case 128: // --version
        std::cout << "Rumur version " << rumur::get_version() << "\n";
        exit(EXIT_SUCCESS);

      default:
        std::cerr << "unexpected error\n";
        exit(EXIT_FAILURE);

    }
  }

  if (optind == argc - 1) {
    struct stat buf;
    if (stat(argv[optind], &buf) < 0) {
      std::cerr << "failed to open " << argv[optind] << ": " << strerror(errno) << "\n";
      exit(EXIT_FAILURE);
    }

    if (S_ISDIR(buf.st_mode)) {
      std::cerr << "failed to open " << argv[optind] << ": this is a directory\n";
      exit(EXIT_FAILURE);
    }

    in_filename = argv[optind];

    auto i = std::make_shared<std::ifstream>(in_filename);
    if (!i->is_open()) {
      std::cerr << "failed to open " << in_filename << "\n";
      exit(EXIT_FAILURE);
    }
    in = i;
  }
}

int main(int argc, char **argv) {

  // parse command line options
  parse_args(argc, argv);

  // parse input model
  rumur::Ptr<rumur::Model> m;
  try {
    m = rumur::parse(in == nullptr ? std::cin : *in);
    resolve_symbols(*m);
    validate(*m);
  } catch (rumur::Error &e) {
    std::cerr << e.loc << ":" << e.what() << "\n";
    return EXIT_FAILURE;
  }

  assert(m != nullptr);

  return EXIT_SUCCESS;
}
