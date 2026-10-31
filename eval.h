#ifndef _EVAL_H_
#define _EVAL_H_
#include "ast.h"

struct value_t *eval(struct ast *);

bool eval_control_pending();
void eval_finish_statement_control();

#endif