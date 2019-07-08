#include <cassert>
#include <cstddef>
#include "except.h"
#include "../log.h"
#include <rumur/rumur.h>
#include "simplify.h"
#include "solver.h"
#include <string>
#include "translate.h"
#include "../utils.h"

using namespace rumur;

namespace smt {

/* Simplification logic. We only attempt to replace tautologies with true and
 * contradictions with false, rather than going further and removing unreachable
 * code. We assume the C compiler building the generated verifier is clever
 * enough to make these transformations itself, so we leave them for it.
 */
namespace { class Simplifier : public BaseTraversal {

 private:
  Solver *solver;

 public:
  Simplifier(Solver &solver_): solver(&solver_) { }

  /* if you are editing the visitation logic, note that the calls to
   * open_scope/close_scope are intended to match the pattern in
   * ../../../librumur/src/resolve-symbols.cc so that the SMT solver resolves
   * ExprIDs/TypeExprIDs the same way we do
   */

  void visit_add(Add &n) final { visit_bexpr(n); }

  void visit_aliasdecl(AliasDecl &n) final {
    dispatch(*n.value);
    simplify(n.value);
  }

  void visit_aliasrule(AliasRule &n) final {
    solver->open_scope();
    for (Ptr<AliasDecl> &alias : n.aliases) {
      dispatch(*alias);
      declare_decl(*alias);
    }
    for (Ptr<Rule> &rule : n.rules)
      dispatch(*rule);
    solver->close_scope();
  }

  void visit_aliasstmt(AliasStmt &n) final {
    solver->open_scope();
    for (Ptr<AliasDecl> &alias : n.aliases) {
      dispatch(*alias);
      declare_decl(*alias);
    }
    for (Ptr<Stmt> &stmt : n.body)
      dispatch(*stmt);
    solver->close_scope();
  }

  void visit_and(And &n) final { visit_bexpr(n); }

  void visit_array(Array &n) final {
    dispatch(*n.index_type);
    dispatch(*n.element_type);
  }

  void visit_assignment(Assignment &n) final {
    dispatch(*n.lhs);
    dispatch(*n.rhs);

    simplify(n.rhs);
  }

  void visit_clear(Clear &n) final {
    dispatch(*n.rhs);

    simplify(n.rhs);
  }

  void visit_constdecl(ConstDecl &n) final {
    dispatch(*n.value);
    simplify(n.value);

    if (n.type != nullptr)
      dispatch(*n.type);
  }

  void visit_div(Div &n) final { visit_bexpr(n); }

  void visit_element(Element &n) final {
    dispatch(*n.array);
    dispatch(*n.index);

    simplify(n.array);
    simplify(n.index);
  }

  void visit_enum(Enum &n) final {

    // boolean is an SMT built-in we don't need to declare
    if (n == *Boolean)
      return;

    throw Unsupported(n.to_string());
  }

  void visit_eq(Eq &n) final { visit_bexpr(n); }
  void visit_errorstmt(ErrorStmt&) final { }

  void visit_exists(Exists &n) final {
    solver->open_scope();
    dispatch(n.quantifier);
    dispatch(*n.expr);

    simplify(n.expr);
    solver->close_scope();
  }

  void visit_exprid(ExprID&) final { }

  void visit_field(Field &n) final {
    dispatch(*n.record);

    simplify(n.record);
  }

  void visit_for(For &n) final {
    solver->open_scope();
    dispatch(n.quantifier);
    for (Ptr<Stmt> &stmt : n.body)
      dispatch(*stmt);
    solver->close_scope();
  }

  void visit_forall(Forall &n) final {
    solver->open_scope();
    dispatch(n.quantifier);
    dispatch(*n.expr);

    simplify(n.expr);
    solver->close_scope();
  }

  void visit_function(Function &n) final {
    solver->open_scope();
    for (Ptr<VarDecl> &parameter : n.parameters) {
      dispatch(*parameter);
      declare_decl(*parameter);
    }
    if (n.return_type != nullptr)
      dispatch(*n.return_type);
    for (Ptr<Decl> &decl : n.decls) {
      dispatch(*decl);
      declare_decl(*decl);
    }
    for (Ptr<Stmt> &stmt : n.body)
      dispatch(*stmt);
    solver->close_scope();
  }

  void visit_functioncall(FunctionCall &n) final {
    for (Ptr<Expr> &arg : n.arguments) {
      dispatch(*arg);
      simplify(arg);
    }
  }

  void visit_geq(Geq &n) final { visit_bexpr(n); }
  void visit_gt(Gt &n) final { visit_bexpr(n); }

  void visit_if(If &n) final {
    for (IfClause &c : n.clauses)
      dispatch(c);
  }

  void visit_ifclause(IfClause &n) final {

    if (n.condition != nullptr)
      dispatch(*n.condition);

    for (Ptr<Stmt> &s : n.body)
      dispatch(*s);

    if (n.condition != nullptr)
      simplify(n.condition);
  }

  void visit_implication(Implication &n) final { visit_bexpr(n); }

  void visit_isundefined(IsUndefined &n) final {
    dispatch(*n.expr);
    simplify(n.expr);
  }

  void visit_leq(Leq &n) final { visit_bexpr(n); }
  void visit_lt(Lt &n) final { visit_bexpr(n); }
  void visit_mod(Mod &n) final { visit_bexpr(n); }

  void visit_model(Model &n) final {
    solver->open_scope();
    for (Ptr<Decl> &decl : n.decls) {
      dispatch(*decl);
      declare_decl(*decl);
    }
    for (Ptr<Function> &function : n.functions) {
      dispatch(*function);
      declare_func(*function);
    }
    for (Ptr<Rule> &rule : n.rules)
      dispatch(*rule);
    solver->open_scope();
  }

  void visit_mul(Mul &n) final { visit_bexpr(n); }
  void visit_negative(Negative &n) final { visit_uexpr(n); }
  void visit_neq(Neq &n) final { visit_bexpr(n); }
  void visit_not(Not &n) final { visit_uexpr(n); }
  void visit_number(Number&) final { }
  void visit_or(Or &n) final { visit_bexpr(n); }

  void visit_procedurecall(ProcedureCall &n) final {
    dispatch(n.call);
  }

  void visit_property(Property&) final {
    /* properties are printed to the user during verification, so don't simplify
     * the expression as it may confuse the user that it's not the same as what
     * they entered
     */
  }

  void visit_propertyrule(PropertyRule &n) final {
    for (Quantifier &quantifier : n.quantifiers)
      dispatch(quantifier);
    for (Ptr<AliasDecl> &alias : n.aliases)
      dispatch(*alias);
    dispatch(n.property);
  }

  void visit_propertystmt(PropertyStmt &n) final {
    dispatch(n.property);
  }

  void visit_put(Put&) final {
    /* deliberately do nothing here because the expression in a 'put' statement
     * is displayed to the user during verification and will surprise them if it
     * has been simplified into something other than what they wrote
     */
  }

  void visit_quantifier(Quantifier &n) final {
    if (n.type != nullptr) {
      dispatch(*n.type);

      declare_var(n.name, *n.type);
    } else {
      assert(n.from != nullptr);
      assert(n.to != nullptr);

      dispatch(*n.from);
      dispatch(*n.to);
      if (n.step != nullptr)
        dispatch(*n.step);

      simplify(n.from);
      simplify(n.to);
      if (n.step != nullptr)
        simplify(n.step);

      const std::string name = mangle(n.name);
      *solver << "(declare-fun " << name << " () Int)\n";
      if (n.from->constant()) {
        const std::string lb = n.from->constant_fold().get_str();
        *solver << "(assert (>= " << name << " " << lb << "))\n";
      }
      if (n.to->constant()) {
        const std::string ub = n.to->constant_fold().get_str();
        *solver << "(assert (<= " << name << " " << ub << "))\n";
      }
    }
  }

  void visit_range(Range &n) final {
    dispatch(*n.min);
    dispatch(*n.max);

    simplify(n.min);
    simplify(n.max);
  }

  void visit_record(Record &n) final {
    for (Ptr<VarDecl> &field : n.fields)
      dispatch(*field);
  }

  void visit_return(Return &n) final {
    if (n.expr != nullptr) {
      dispatch(*n.expr);

      simplify(n.expr);
    }
  }

  void visit_ruleset(Ruleset &n) final {
    solver->open_scope();
    for (Quantifier &quantifier : n.quantifiers)
      dispatch(quantifier);
    for (Ptr<AliasDecl> &alias : n.aliases)
      dispatch(*alias);
    for (Ptr<Rule> &rule : n.rules)
      dispatch(*rule);
    solver->close_scope();
  }

  void visit_scalarset(Scalarset &n) final {
    dispatch(*n.bound);
    simplify(n.bound);
  }

  void visit_simplerule(SimpleRule &n) final {
    solver->open_scope();
    for (Quantifier &quantifier : n.quantifiers)
      dispatch(quantifier);
    for (Ptr<AliasDecl> &alias : n.aliases)
      dispatch(*alias);
    if (n.guard != nullptr) {
      dispatch(*n.guard);
      simplify(n.guard);
    }
    for (Ptr<Decl> &decl : n.decls) {
      dispatch(*decl);
      declare_decl(*decl);
    }
    for (Ptr<Stmt> &stmt : n.body)
      dispatch(*stmt);
    solver->close_scope();
  }

  void visit_startstate(StartState &n) final {
    solver->open_scope();
    for (Quantifier &quantifier : n.quantifiers)
      dispatch(quantifier);
    for (Ptr<AliasDecl> &alias : n.aliases)
      dispatch(*alias);
    for (Ptr<Decl> &decl : n.decls) {
      dispatch(*decl);
      declare_decl(*decl);
    }
    for (Ptr<Stmt> &stmt : n.body)
      dispatch(*stmt);
    solver->close_scope();
  }

  void visit_sub(Sub &n) final { visit_bexpr(n); }

  void visit_switchcase(SwitchCase &n) final {
    for (Ptr<Expr> &match : n.matches) {
      dispatch(*match);
      simplify(match);
    }
    for (Ptr<Stmt> &stmt : n.body) {
      dispatch(*stmt);
    }
  }

  void visit_switch(Switch &n) final {
    dispatch(*n.expr);
    simplify(n.expr);

    for (SwitchCase &c : n.cases)
      dispatch(c);
  }

  void visit_ternary(Ternary &n) final {
    dispatch(*n.cond);
    dispatch(*n.lhs);
    dispatch(*n.rhs);

    simplify(n.cond);
    simplify(n.lhs);
    simplify(n.rhs);
  }

  void visit_typedecl(TypeDecl &n) final {
    dispatch(*n.value);
  }

  void visit_typeexprid(TypeExprID &n) final {
    dispatch(*n.referent);
  }

  void visit_undefine(Undefine &n) final {
    dispatch(*n.rhs);
  }

  void visit_vardecl(VarDecl &n) final {
    dispatch(*n.type);
  }

  void visit_while(While &n) final {
    dispatch(*n.condition);
    simplify(n.condition);

    for (Ptr<Stmt> &stmt : n.body)
      dispatch(*stmt);
  }

 private:

  void visit_bexpr(BinaryExpr &n) {
    dispatch(*n.lhs);
    dispatch(*n.rhs);

    simplify(n.lhs);
    simplify(n.rhs);
  }

  void visit_uexpr(UnaryExpr &n) {
    dispatch(*n.rhs);

    simplify(n.rhs);
  }

  // try to squash a boolean expression to "True" or "False"
  void simplify(Ptr<Expr> &e) {

    assert (e != nullptr && "attempt to simplify a NULL expression");

    // we currently only handle boolean expressions
    if (!e->is_boolean())
      return;

    // there's no point trying to simplify a literal we will replace with itself
    if (*e == *True || *e == *False)
      return;

    std::string claim;
    try {
      claim = translate(*e);
    } catch (Unsupported&) {
      *info << "skipping SMT simplification of unsupported expression \""
        << e->to_string() << "\"\n";
      return;
    }

    if (solver->is_true(claim)) {
      *info << "simplifying \"" << e->to_string() << "\" to true\n";
      e = make_true();
    } else if (solver->is_false(claim)) {
      *info << "simplifying \"" << e->to_string() << "\" to false\n";
      e = make_false();
    }
  }

  // invent a reference to "true"
  static Ptr<Expr> make_true(void) {
    return Ptr<Expr>(True);
  }

  // invent a reference to "false"
  static Ptr<Expr> make_false(void) {
    return Ptr<Expr>(False);
  }

  // declare a variable/type to the solver
  void declare_decl(const Decl &decl) {

    if (auto v = dynamic_cast<const VarDecl*>(&decl)) {
      declare_var(v->name, *v->type);
    } else {
      // TODO
      throw Unsupported();
    }
  }

  void declare_var(const std::string &name, const TypeExpr &type) {

    const Ptr<TypeExpr> t = type.resolve();

    const std::string n = mangle(name);

    // the solver already knows boolean, so we can just declare it
    if (*t == *Boolean) {
      *solver << "(declare-fun " << mangle(name) << " () Bool)\n";

    } else if (isa<Range>(t)) {
      *solver << "(declare-fun " << n << " () Int)\n";

      // if this range's bounds are static, make them known to the solver
      auto r = dynamic_cast<const Range&>(*t);
      if (r.constant()) {
        const std::string lb = r.min->constant_fold().get_str();
        const std::string ub = r.max->constant_fold().get_str();
        *solver
          << "(assert (>= " << n << " " << lb << "))\n"
          << "(assert (<= " << n << " " << ub << "))\n";
      }

    } else if (isa<Scalarset>(t)) {
      /* A scalarset is nothing special to the SMT solver. That is, we just
       * declare it as an integer and don't expect the solver to use any
       * symmetry reasoning.
       */
      *solver
        << "(declare-fun " << n << " () Int)\n"
        << "(assert (>= " << n << " 0))\n";

      // if this scalarset's bounds are static, make them known to the solver
      auto s = dynamic_cast<const Scalarset&>(*t);
      if (s.constant()) {
        const std::string bound = s.bound->constant_fold().get_str();
        *solver << "(assert (< " << n << " " << bound << "))\n";
      }

    } else {
      // TODO
      throw Unsupported();
    }
  }

  void declare_func(const Function&) {
    throw Unsupported();
  }
}; }

void simplify(Model &m) {

  // establish our connection to the solver
  Solver solver;

  // recursively traverse the model, simplifying as we go
  Simplifier simplifier(solver);
  simplifier.dispatch(m);
}

}