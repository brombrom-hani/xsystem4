/* Copyright (C) 2019 Nunuhara Cabbage <nunuhara@haniwa.technology>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "system4.h"
#include "system4/ain.h"
#include "system4/string.h"
#include "ainedit.h"
#include "jaf.h"

static void define_types(struct ain *ain, struct jaf_block_item *decl);
static void jaf_resolve_types(struct ain *ain, struct jaf_block *block);
static void jaf_process_declarations(struct ain *ain, struct jaf_block *block);
static void jaf_analyze_block(struct jaf_env *env, struct jaf_block *block);

void jaf_define_struct(struct ain *ain, struct jaf_type_specifier *type)
{
	if (!type->name)
		ERROR("Anonymous structs not supported");

	char *u = encode_text_to_input_format(type->name->text);
	if (ain_get_struct(ain, u)) {
		ERROR("Redefining structs not supported");
	}

	type->struct_no = ain_add_struct(ain, u);
	free(u);

	for (size_t i = 0; i < type->def->nr_items; i++) {
		define_types(ain, type->def->items[i]);
	}
}

static void define_types(struct ain *ain, struct jaf_block_item *decl)
{
	if (!decl)
		ERROR("DECL IS NULL");
	if (!decl->decl.type)
		ERROR("TYPE IS NULL");
	if (decl->decl.type->type != JAF_STRUCT)
		return;

	jaf_define_struct(ain, decl->decl.type);
}

static enum ain_data_type jaf_to_ain_data_type(enum jaf_type type)
{
	switch (type) {
	case JAF_VOID:   return AIN_VOID;
	case JAF_INT:    return AIN_INT;
	case JAF_FLOAT:  return AIN_FLOAT;
	case JAF_STRING: return AIN_STRING;
	case JAF_STRUCT: return AIN_STRUCT;
	case JAF_ENUM:   ERROR("Enums not supported");
	default:         ERROR("Unknown type: %d", type);
	}
}

static void jaf_to_ain_type(possibly_unused struct ain *ain, struct ain_type *out, struct jaf_type_specifier *in)
{
	out->data = jaf_to_ain_data_type(in->type);
	if (in->type == JAF_STRUCT) {
		out->struc = in->struct_no;
	}
}

static void resolve_typedef(struct ain *ain, struct jaf_type_specifier *type)
{
	char *u = encode_text_to_input_format(type->name->text);
	int sno = ain_get_struct_no(ain, u);
	if (sno) {
		type->type = JAF_STRUCT;
		type->struct_no = sno;
	} else {
		ERROR("Failed to resolve typedef \"%s\"", type->name->text);
	}

	free(u);
}

static void jaf_to_initval(struct ain_initval *dst, struct jaf_expression *expr)
{
	switch (expr->type) {
	case JAF_EXP_INT:
		dst->data_type = AIN_INT;
		dst->int_value = expr->i;
		break;
	case JAF_EXP_FLOAT:
		dst->data_type = AIN_FLOAT;
		dst->float_value = expr->f;
		break;
	case JAF_EXP_STRING:
		dst->data_type = AIN_STRING;
		dst->string_value = strdup(expr->s->text);
		break;
	default:
		ERROR("Initval is not constant");
	}
}

static void analyze_expression(struct jaf_env *env, struct jaf_expression **expr)
{
	if (!*expr)
		return;
	jaf_derive_types(env, *expr);
	*expr = jaf_simplify(*expr);
}

static void analyze_block(struct jaf_env *env, struct jaf_block *block);

static void analyze_global_declaration(struct jaf_env *env, struct jaf_declaration *decl)
{
	if (!decl->init)
		return;
	analyze_expression(env, &decl->init);
	assert(decl->type);
	jaf_check_type(decl->init, decl->type);
	// add initval to ain object
	struct ain_initval init = { .global_index = decl->var_no };
	jaf_to_initval(&init, decl->init);
	ain_add_initval(env->ain, &init);
}

static void analyze_local_declaration(struct jaf_env *env, struct jaf_declaration *decl)
{
	assert(env->func_no >= 0 && env->func_no < env->ain->nr_functions);
	assert(decl->var_no >= 0 && decl->var_no < env->ain->functions[env->func_no].nr_vars);
	// add local to environment
	env->locals = xrealloc_array(env->locals, env->nr_locals, env->nr_locals+1, sizeof(struct ain_variable*));
	env->locals[env->nr_locals++] = &env->ain->functions[env->func_no].vars[decl->var_no];
}

static void analyze_function(struct jaf_env *env, struct jaf_declaration *decl)
{
	// create new scope with function arguments
	assert(decl->func_no < env->ain->nr_functions);
	struct ain_function *fun = &env->ain->functions[decl->func_no];
	struct jaf_env *funenv = xcalloc(1, sizeof(struct jaf_env));
	funenv->ain = env->ain;
	funenv->parent = env;
	funenv->func_no = decl->func_no;
	funenv->fundecl = decl;
	funenv->nr_locals = fun->nr_args;
	funenv->locals = xcalloc(funenv->nr_locals, sizeof(struct ain_variable*));
	for (size_t i = 0; i < funenv->nr_locals; i++) {
		funenv->locals[i] = &fun->vars[i];
	}

	jaf_analyze_block(funenv, decl->body);
	free(funenv->locals);
	free(funenv);
}

static void analyze_statement(struct jaf_env *env, struct jaf_block_item *item)
{
	if (!item)
		return;
	switch (item->kind) {
	case JAF_DECLARATION:
		if (env->parent)
			analyze_local_declaration(env, &item->decl);
		else
			analyze_global_declaration(env, &item->decl);
		break;
	case JAF_FUNDECL:
		analyze_function(env, &item->decl);
		break;
	case JAF_STMT_LABELED:
		analyze_statement(env, item->label.stmt);
		break;
	case JAF_STMT_COMPOUND:
		analyze_block(env, item->block);
		break;
	case JAF_STMT_EXPRESSION:
		analyze_expression(env, &item->expr);
		break;
	case JAF_STMT_IF:
		analyze_expression(env, &item->cond.test);
		analyze_statement(env, item->cond.consequent);
		analyze_statement(env, item->cond.alternative);
		break;
	case JAF_STMT_SWITCH:
		analyze_expression(env, &item->swi.expr);
		analyze_block(env, item->swi.body);
		break;
	case JAF_STMT_WHILE:
	case JAF_STMT_DO_WHILE:
		analyze_expression(env, &item->while_loop.test);
		analyze_statement(env, item->while_loop.body);
		break;
	case JAF_STMT_FOR:
		analyze_block(env, item->for_loop.init);
		analyze_expression(env, &item->for_loop.test);
		analyze_expression(env, &item->for_loop.after);
		analyze_statement(env, item->for_loop.body);
		break;
	case JAF_STMT_RETURN:
		analyze_expression(env, &item->expr);
		jaf_check_type(item->expr, env->fundecl->type);
		break;
	case JAF_STMT_CASE:
	case JAF_STMT_DEFAULT:
		analyze_statement(env, item->swi_case.stmt);
		break;
	case JAF_STMT_GOTO:
	case JAF_STMT_CONTINUE:
	case JAF_STMT_BREAK:
		break;
	}
}

static void jaf_analyze_block(struct jaf_env *env, struct jaf_block *block)
{
	for (size_t i = 0; i < block->nr_items; i++) {
		analyze_statement(env, block->items[i]);
	}
}

static void analyze_block(struct jaf_env *env, struct jaf_block *block)
{
	struct jaf_env *blockenv = xcalloc(1, sizeof(struct jaf_env));
	blockenv->ain = env->ain;
	blockenv->parent = env;
	blockenv->func_no = env->func_no;
	blockenv->fundecl = env->fundecl;

	jaf_analyze_block(blockenv, block);
	free(blockenv->locals);
	free(blockenv);
}

static void resolve_decl_types(struct ain *ain, struct jaf_declaration *decl)
{
	if (decl->type->type == JAF_TYPEDEF)
		resolve_typedef(ain, decl->type);
	if (decl->type->type != JAF_STRUCT || !decl->type->def)
		return;

	assert(decl->type->struct_no >= 0); // set in parse phase
	assert(decl->type->struct_no < ain->nr_structures);

	// create struct definition in ain object
	struct jaf_block *def = decl->type->def;
	struct ain_variable *members = xcalloc(def->nr_items, sizeof(struct ain_variable));
	for (size_t i = 0; i < def->nr_items; i++) {
		members[i].name = encode_text_to_input_format(def->items[i]->decl.name->text);
		if (ain->version >= 12)
			members[i].name2 = strdup("");
		jaf_to_ain_type(ain, &members[i].type, def->items[i]->decl.type);
	}

	struct ain_struct *s = &ain->structures[decl->type->struct_no];
	s->nr_members = def->nr_items;
	s->members = members;
}

static void resolve_statement_types(struct ain *ain, struct jaf_block_item *item)
{
	switch (item->kind) {
	case JAF_DECLARATION:
		resolve_decl_types(ain, &item->decl);
		break;
	case JAF_FUNDECL:
		if (item->decl.params)
			jaf_resolve_types(ain, item->decl.params);
		jaf_resolve_types(ain, item->decl.body);
		break;
	case JAF_STMT_LABELED:
		resolve_statement_types(ain, item->label.stmt);
		break;
	case JAF_STMT_COMPOUND:
		jaf_resolve_types(ain, item->block);
		break;
	case JAF_STMT_IF:
		resolve_statement_types(ain, item->cond.consequent);
		resolve_statement_types(ain, item->cond.alternative);
		break;
	case JAF_STMT_SWITCH:
		jaf_resolve_types(ain, item->swi.body);
		break;
	case JAF_STMT_WHILE:
	case JAF_STMT_DO_WHILE:
		resolve_statement_types(ain, item->while_loop.body);
		break;
	case JAF_STMT_FOR:
		jaf_resolve_types(ain, item->for_loop.init);
		resolve_statement_types(ain, item->for_loop.body);
		break;
	case JAF_STMT_CASE:
	case JAF_STMT_DEFAULT:
		resolve_statement_types(ain, item->swi_case.stmt);
		break;
	case JAF_STMT_EXPRESSION:
	case JAF_STMT_GOTO:
	case JAF_STMT_CONTINUE:
	case JAF_STMT_BREAK:
	case JAF_STMT_RETURN:
		break;
	}
}

static void jaf_resolve_types(struct ain *ain, struct jaf_block *block)
{
	for (size_t i = 0; i < block->nr_items; i++) {
		resolve_statement_types(ain, block->items[i]);
	}
}

static void init_variable(struct ain *ain, struct ain_variable *var, struct jaf_declaration *decl, int var_no)
{
	var->name = encode_text_to_input_format(decl->name->text);
	if (ain->version >= 12)
		var->name2 = strdup("");
	jaf_to_ain_type(ain, &var->type, decl->type);
	decl->var_no = var_no;
}

static struct ain_variable *block_get_vars(struct ain *ain, struct jaf_block *block, struct ain_variable *vars, int *nr_vars);

static struct ain_variable *block_item_get_vars(struct ain *ain, struct jaf_block_item *item, struct ain_variable *vars, int *nr_vars)
{
	switch (item->kind) {
	case JAF_DECLARATION:
		if (!item->decl.name)
			break;
		vars = xrealloc_array(vars, *nr_vars, *nr_vars + 1, sizeof(struct ain_variable));
		init_variable(ain, vars + *nr_vars, &item->decl, *nr_vars);
		(*nr_vars)++;
		break;
	case JAF_STMT_LABELED:
		vars = block_item_get_vars(ain, item->label.stmt, vars, nr_vars);
		break;
	case JAF_STMT_COMPOUND:
		vars = block_get_vars(ain, item->block, vars, nr_vars);
		break;
	case JAF_STMT_IF:
		vars = block_item_get_vars(ain, item->cond.consequent, vars, nr_vars);
		vars = block_item_get_vars(ain, item->cond.alternative, vars, nr_vars);
		break;
	case JAF_STMT_SWITCH:
		vars = block_get_vars(ain, item->swi.body, vars, nr_vars);
		break;
	case JAF_STMT_WHILE:
	case JAF_STMT_DO_WHILE:
		vars = block_item_get_vars(ain, item->while_loop.body, vars, nr_vars);
		break;
	case JAF_STMT_FOR:
		vars = block_get_vars(ain, item->for_loop.init, vars, nr_vars);
		vars = block_item_get_vars(ain, item->for_loop.body, vars, nr_vars);
		break;
	case JAF_STMT_CASE:
	case JAF_STMT_DEFAULT:
		vars = block_item_get_vars(ain, item->swi_case.stmt, vars, nr_vars);
		break;

	case JAF_STMT_EXPRESSION:
	case JAF_STMT_GOTO:
	case JAF_STMT_CONTINUE:
	case JAF_STMT_BREAK:
	case JAF_STMT_RETURN:
		break;

	case JAF_FUNDECL:
		ERROR("Nested functions not supported");
	}
	return vars;
}

static struct ain_variable *block_get_vars(struct ain *ain, struct jaf_block *block, struct ain_variable *vars, int *nr_vars)
{
	for (size_t i = 0; i < block->nr_items; i++) {
		vars = block_item_get_vars(ain, block->items[i], vars, nr_vars);
	}
	return vars;
}

static void function_init_vars(struct ain *ain, struct ain_function *f, struct jaf_declaration *decl)
{
	f->nr_args = decl->params ? decl->params->nr_items : 0;
	f->nr_vars = f->nr_args;
	f->vars = xcalloc(f->nr_args, sizeof(struct ain_variable));
	for (int i = 0; i < f->nr_args; i++) {
		assert(decl->params->items[i]->kind == JAF_DECLARATION);
		assert(decl->params->items[i]->decl.name);
		struct jaf_declaration *param = &decl->params->items[i]->decl;
		init_variable(ain, &f->vars[i], param, i);
	}
	f->vars = block_get_vars(ain, decl->body, f->vars, &f->nr_vars);
}

static void add_function(struct ain *ain, struct jaf_declaration *decl)
{
	struct ain_function f = {0};
	f.name = strdup(decl->name->text);
	jaf_to_ain_type(ain, &f.return_type, decl->type);
	function_init_vars(ain, &f, decl);

	decl->func_no = ain_add_function(ain, &f);
}

static void add_global(struct ain *ain, struct jaf_declaration *decl)
{
	char *u = encode_text_to_input_format(decl->name->text);
	struct ain_variable *v = ain_add_global(ain, u);
	jaf_to_ain_type(ain, &v->type, decl->type);
	decl->var_no = v - ain->globals;
	free(u);
}

static void jaf_process_declarations(struct ain *ain, struct jaf_block *block)
{
	for (size_t i = 0; i < block->nr_items; i++) {
		if (block->items[i]->decl.name) {
			if (block->items[i]->kind == JAF_FUNDECL)
				add_function(ain, &block->items[i]->decl);
			else
				add_global(ain, &block->items[i]->decl);
		}
	}
}

struct jaf_block *jaf_static_analyze(struct ain *ain, struct jaf_block *block)
{
	struct jaf_env env = {
		.ain = ain,
		.parent = NULL
	};

	// pass 1: typedefs && struct definitions
	jaf_resolve_types(ain, block);
	// pass 2: register globals (names, types)
	jaf_process_declarations(ain, block);
	// pass 3: type analysis & simplification & global initvals
	jaf_analyze_block(&env, block);

	return block;
}
