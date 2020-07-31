#include <cassert>
#include <cstddef>
#include <iostream>
#include "location.hh"
#include <memory>
#include <rumur/Decl.h>
#include <rumur/except.h>
#include <rumur/Expr.h>
#include <rumur/Property.h>
#include <rumur/Ptr.h>
#include <rumur/Rule.h>
#include <rumur/Stmt.h>
#include <rumur/traverse.h>
#include <string>
#include "utils.h"
#include <vector>

namespace rumur {

namespace {
  /* A traversal pass that checks any return statements within a rule do not
   * have a trailing expression.
   */
  class ReturnChecker : public ConstTraversal {

   public:
    /* Avoid recursing into functions, that may have return statements with an
     * expression.
     */
    void visit_function(const Function&) final { }
    void visit_functioncall(const FunctionCall&) final { }
    void visit_procedurecall(const ProcedureCall&) final { }

    void visit_return(const Return &n) final {
      if (n.expr != nullptr)
        throw Error("return statement in rule or startstate returns a value",
          n.loc);

      // No need to recurse into the return statement's child.
    }

    static void check(const Node &n) {
      ReturnChecker c;
      c.dispatch(n);
    }

    virtual ~ReturnChecker() = default;
  };
}

Rule::Rule(const std::string &name_, const location &loc_):
  Node(loc_), name(name_) { }

std::vector<Ptr<Rule>> Rule::flatten() const {
  return { Ptr<Rule>(clone()) };
}

AliasRule::AliasRule(const std::vector<Ptr<AliasDecl>> &aliases_,
  const std::vector<Ptr<Rule>> &rules_, const location &loc_):
  Rule("", loc_), rules(rules_) {

  aliases = aliases_;
}

AliasRule *AliasRule::clone() const {
  return new AliasRule(*this);
}

std::vector<Ptr<Rule>> AliasRule::flatten() const {
  std::vector<Ptr<Rule>> rs;
  for (const Ptr<Rule> &r : rules) {
    for (Ptr<Rule> &f : r->flatten()) {
      f->aliases.insert(f->aliases.begin(), aliases.begin(), aliases.end());
      rs.push_back(f);
    }
  }
  return rs;
}

SimpleRule::SimpleRule(const std::string &name_, const Ptr<Expr> &guard_,
  const std::vector<Ptr<Decl>> &decls_,
  const std::vector<Ptr<Stmt>> &body_, const location &loc_):
  Rule(name_, loc_), guard(guard_), decls(decls_), body(body_) { }

SimpleRule *SimpleRule::clone() const {
  return new SimpleRule(*this);
}

void SimpleRule::validate() const {
  ReturnChecker::check(*this);
}

StartState::StartState(const std::string &name_,
  const std::vector<Ptr<Decl>> &decls_,
  const std::vector<Ptr<Stmt>> &body_, const location &loc_):
  Rule(name_, loc_), decls(decls_), body(body_) { }

StartState *StartState::clone() const {
  return new StartState(*this);
}

void StartState::validate() const {
  ReturnChecker::check(*this);
}

PropertyRule::PropertyRule(const std::string &name_, const Property &property_,
  const location &loc_):
  Rule(name_, loc_), property(property_) { }

PropertyRule *PropertyRule::clone() const {
  return new PropertyRule(*this);
}

Ruleset::Ruleset(const std::vector<Quantifier> &quantifiers_,
  const std::vector<Ptr<Rule>> &rules_, const location &loc_):
  Rule("", loc_), rules(rules_) {
  quantifiers = quantifiers_;
}

Ruleset *Ruleset::clone() const {
  return new Ruleset(*this);
}

void Ruleset::validate() const {
  for (const Quantifier &q : quantifiers) {
    if (!q.constant())
      throw Error("non-constant quantifier expression as ruleset parameter",
        q.loc);
  }
}

std::vector<Ptr<Rule>> Ruleset::flatten() const {
  std::vector<Ptr<Rule>> rs;
  for (const Ptr<Rule> &r : rules) {
    for (Ptr<Rule> &f : r->flatten()) {
      for (const Quantifier &q : quantifiers)
        f->quantifiers.push_back(q);
      rs.push_back(f);
    }
  }
  return rs;
}

}
