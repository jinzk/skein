#ifndef SKEIN_DECORATORS_H
#define SKEIN_DECORATORS_H

struct ast;
struct funcdef;
struct impl_node;

/*
 * Lower @decorator attributes on impl methods into ordinary AST:
 *
 *   let $body$Type$method$line$column$index = <original function>;
 *   let $dec$Type$method$line$column$index: callable = <decorator chain>;
 *   impl Type {
 *     func method(self, ...) { return $dec(self, ...); }
 *   }
 *
 * Generator methods use the same ordinary forwarder: it returns the generator
 * produced by the decorator chain.  The forwarder is never marked generator,
 * so iteration uses the
 * decorator result directly.
 *
 * Returns the original impl when nothing needs lowering, otherwise an 'L'
 * chain that evaluates the hidden bindings before the impl declaration.
 * Validation errors are reported through yyerror; in that case the original
 * impl is returned unchanged.
 */
struct ast *lower_impl_decorators(struct impl_node *impl);

/*
 * Lower decorators on a named function declaration:
 *
 *   let $d0: callable = <decorator expression 0>;
 *   let $d1: callable = <decorator expression 1>;
 *   func f(...) { ... }
 *   let $dec: callable = $d0($d1(f));
 *   f = $dec;
 *
 * Decorator expressions are evaluated before the function binding, and
 * applied from the inside out afterwards.  Returns the original funcdef when
 * there are no decorators.
 */
struct ast *lower_function_decorators(struct ast *decorators,
                                      struct funcdef *func);

#endif
