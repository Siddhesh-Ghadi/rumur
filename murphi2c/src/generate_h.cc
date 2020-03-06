#include <cstddef>
#include "CodeGenerator.h"
#include "generate_c.h"
#include "generate_h.h"
#include <iostream>
#include "resources.h"
#include <rumur/rumur.h>

using namespace rumur;

namespace {

class HGenerator : public CodeGenerator, public ConstTraversal {

 private:
  std::ostream &out;

 public:
  HGenerator(std::ostream &out_, bool): out(out_) { }

  // helpers to make output below more natural

  HGenerator &operator<<(const std::string &s) {
    out << s;
    return *this;
  }

  HGenerator &operator<<(const Node &n) {
    dispatch(n);
    return *this;
  }

  void visit_model(const Model &n) final {

    // emit declarations
    for (const Ptr<Decl> &d : n.decls)
      *this << *d;
  }

  void visit_typeexprid(const TypeExprID &n) final {
    // FIXME: ideally we just want to call CGenerator::visit_typeexprid() here
    *this << n.name;
  }

  void visit_vardecl(const VarDecl &n) final {
    *this << indentation() << "extern " << *n.type << " " << n.name << ";\n";
  }
};

}

void generate_h(const Node &n, bool pack, std::ostream &out) {

  // write the static prefix to the beginning of the source file
  for (size_t i = 0; i < resources_h_prefix_h_len; i++)
    out << (char)resources_h_prefix_h[i];

  HGenerator gen(out, pack);
  gen.dispatch(n);

  // close the `extern "C"` block opened in ../resources/h_prefix.h
  out
    << "\n"
    << "#ifdef __cplusplus\n"
    << "}\n"
    << "#endif\n";
}
