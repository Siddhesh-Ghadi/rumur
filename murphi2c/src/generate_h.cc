#include <cstddef>
#include "CLikeGenerator.h"
#include "generate_h.h"
#include <iostream>
#include "options.h"
#include "resources.h"
#include <rumur/rumur.h>
#include <string>

using namespace rumur;

namespace {

class HGenerator : public CLikeGenerator {

 public:
  HGenerator(std::ostream &out_, bool pack_): CLikeGenerator(out_, pack_) { }

  void visit_constdecl(const ConstDecl &n) final {
    *this << indentation() << "extern const ";

    // replicate the logic from CGenerator::visit_constdecl
    if (n.type != nullptr) {
      *this << *n.type;
    } else {
      const Ptr<TypeExpr> type = n.value->type();
      auto it = enum_typedefs.find(type->unique_id);
      if (it != enum_typedefs.end()) {
        *this << it->second;
      } else {
        *this << "__typeof__(" << *n.value << ")";
      }
    }

    *this << " " << n.name << ";\n";
  }

  void visit_function(const Function &n) final {
    *this << indentation();
    if (n.return_type == nullptr) {
      *this << "void";
    } else {
      *this << *n.return_type;
    }
    *this << " " << n.name << "(";
    if (n.parameters.empty()) {
      *this << "void";
    } else {
      std::string sep;
      for (const Ptr<VarDecl> &p : n.parameters) {
        *this << sep << *p->type << " ";
        // if this is a var parameter, it needs to be a pointer
        if (!p->readonly) {
          *this << "*" << p->name << "_";
        } else {
          *this << p->name;
        }
        sep = ", ";
      }
    }
    *this << ");\n";
  }

  void visit_propertyrule(const PropertyRule &n) final {

    // function prototype
    *this << indentation() << "bool " << n.name << "(";

    // parameters
    if (n.quantifiers.empty()) {
      *this << "void";
    } else {
      std::string sep;
      for (const Quantifier &q : n.quantifiers) {
        *this << sep;
        if (auto t = dynamic_cast<const TypeExprID*>(q.type.get())) {
          *this << t->name;
        } else {
          *this << value_type;
        }
        *this << " " << q.name;
        sep = ", ";
      }
    }

    *this << ");\n";
  }

  void visit_simplerule(const SimpleRule &n) final {
    *this << indentation() << "bool guard_" << n.name << "(";

    // parameters
    if (n.quantifiers.empty()) {
      *this << "void";
    } else {
      std::string sep;
      for (const Quantifier &q : n.quantifiers) {
        *this << sep;
        if (auto t = dynamic_cast<const TypeExprID*>(q.type.get())) {
          *this << t->name;
        } else {
          *this << value_type;
        }
        *this << " " << q.name;
        sep = ", ";
      }
    }

    *this << ");\n\n";

    *this << indentation() << "void rule_" << n.name << "(";

    // parameters
    if (n.quantifiers.empty()) {
      *this << "void";
    } else {
      std::string sep;
      for (const Quantifier &q : n.quantifiers) {
        *this << sep;
        if (auto t = dynamic_cast<const TypeExprID*>(q.type.get())) {
          *this << t->name;
        } else {
          *this << value_type;
        }
        *this << " " << q.name;
        sep = ", ";
      }
    }

    *this << ");\n";
  }

  void visit_startstate(const StartState &n) final {
    *this << indentation() << "void startstate_" << n.name << "(";

    // parameters
    if (n.quantifiers.empty()) {
      *this << "void";
    } else {
      std::string sep;
      for (const Quantifier &q : n.quantifiers) {
        *this << sep;
        if (auto t = dynamic_cast<const TypeExprID*>(q.type.get())) {
          *this << t->name;
        } else {
          *this << value_type;
        }
        *this << " " << q.name;
        sep = ", ";
      }
    }

    *this << ");\n";
  }

  void visit_vardecl(const VarDecl &n) final {
    *this << indentation();
    if (n.is_in_state())
      *this << "extern ";
    *this << *n.type << " " << n.name << ";\n";
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
