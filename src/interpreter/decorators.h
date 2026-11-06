#ifndef SKEIN_DECORATORS_H
#define SKEIN_DECORATORS_H

struct ast;
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
 * Returns the original impl when nothing needs lowering, otherwise an 'L'
 * chain that evaluates the hidden bindings before the impl declaration.
 * Validation errors are reported through yyerror; in that case the original
 * impl is returned unchanged.
 */
struct ast *lower_impl_decorators(struct impl_node *impl);

#endif
