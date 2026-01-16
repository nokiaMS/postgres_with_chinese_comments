/*-------------------------------------------------------------------------
 *
 * prepqual.c
 *	  Routines for preprocessing qualification expressions
 *
 *
 * While the parser will produce flattened (N-argument) AND/OR trees from
 * simple sequences of AND'ed or OR'ed clauses, there might be an AND clause
 * directly underneath another AND, or OR underneath OR, if the input was
 * oddly parenthesized.  Also, rule expansion and subquery flattening could
 * produce such parsetrees.  The planner wants to flatten all such cases
 * to ensure consistent optimization behavior.
 *
 * Formerly, this module was responsible for doing the initial flattening,
 * but now we leave it to eval_const_expressions to do that since it has to
 * make a complete pass over the expression tree anyway.  Instead, we just
 * have to ensure that our manipulations preserve AND/OR flatness.
 * pull_ands() and pull_ors() are used to maintain flatness of the AND/OR
 * tree after local transformations that might introduce nested AND/ORs.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/prep/prepqual.c
 *
 *-------------------------------------------------------------------------
 */
/*-------------------------------------------------------------------------
 *
 * prepqual.c
 *    限定表达式预处理相关的函数实现文件
 *
 * 虽然语法解析器会将简单的、连续的 AND/OR 子句序列，解析为扁平化的（支持 N 个参数的）AND/OR 表达式树；
 * 但如果输入的 SQL 语句使用了不规范的括号分组，或者在规则展开、子查询扁平化的过程中，都可能产生特殊的表达式结构——
 * 即一个 AND 子句直接嵌套在另一个 AND 子句之下，或一个 OR 子句直接嵌套在另一个 OR 子句之下。
 * 为了保证查询优化行为的一致性，查询优化器需要将这类嵌套结构全部扁平化。
 *
 * 该模块在早期版本中负责执行初始的扁平化操作，不过现在我们将这项工作交由 `eval_const_expressions` 函数完成；
 * 原因是该函数本身就需要对整个表达式树进行完整遍历，顺带处理扁平化可以避免重复遍历，提升效率。
 * 目前该模块的核心职责，是确保在对表达式进行局部转换操作后，依然能够维持 AND/OR 表达式树的扁平化特性。
 * 其中 `pull_ands()` 和 `pull_ors()` 两个函数的作用是：当局部转换操作引入新的嵌套 AND/OR 结构时，对其进行扁平化处理，以维持结构一致性。
 *
 * 版权声明部分
 * 本文件相关代码的版权归 PostgreSQL 全球开发组所有（1996-2025）
 * 部分代码的版权归加州大学董事会所有（1994）
 *
 * 源码标识
 *    该文件在 PostgreSQL 源码树中的路径为：src/backend/optimizer/prep/prepqual.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"


static List *pull_ands(List *andlist);
static List *pull_ors(List *orlist);
static Expr *find_duplicate_ors(Expr *qual, bool is_check);
static Expr *process_duplicate_ors(List *orlist);


/*
 * negate_clause
 *	  Negate a Boolean expression.
 *
 * Input is a clause to be negated (e.g., the argument of a NOT clause).
 * Returns a new clause equivalent to the negation of the given clause.
 *
 * Although this can be invoked on its own, it's mainly intended as a helper
 * for eval_const_expressions(), and that context drives several design
 * decisions.  In particular, if the input is already AND/OR flat, we must
 * preserve that property.  We also don't bother to recurse in situations
 * where we can assume that lower-level executions of eval_const_expressions
 * would already have simplified sub-clauses of the input.
 *
 * The difference between this and a simple make_notclause() is that this
 * tries to get rid of the NOT node by logical simplification.  It's clearly
 * always a win if the NOT node can be eliminated altogether.  However, our
 * use of DeMorgan's laws could result in having more NOT nodes rather than
 * fewer.  We do that unconditionally anyway, because in WHERE clauses it's
 * important to expose as much top-level AND/OR structure as possible.
 * Also, eliminating an intermediate NOT may allow us to flatten two levels
 * of AND or OR together that we couldn't have otherwise.  Finally, one of
 * the motivations for doing this is to ensure that logically equivalent
 * expressions will be seen as physically equal(), so we should always apply
 * the same transformations.
 */
/*
 * negate_clause
 *    对一个布尔表达式进行取反操作。
 *
 * 输入参数是需要被取反的子句（例如，NOT 子句的参数）。
 * 返回一个新的子句，该子句与输入子句的取反结果在逻辑上等价。
 *
 * 虽然该函数可以独立调用，但它的主要用途是作为 `eval_const_expressions()` 函数的辅助函数，
 * 并且这一使用场景决定了它的多项设计决策。具体来说：如果输入子句已经是扁平化的 AND/OR 结构，
 * 那么我们必须保留这一结构特性。此外，在某些场景下我们无需进行递归处理——因为可以假定，
 * `eval_const_expressions()` 函数在底层执行时，已经对输入子句的子节点进行了简化处理。
 *
 * 该函数与简单调用 `make_notclause()` 函数的区别在于：它会尝试通过逻辑简化操作消除 NOT 节点。
 * 显然，如果能够彻底移除 NOT 节点，这无论如何都是更优的选择。不过，我们运用德·摩根定律进行转换时，
 * 有可能导致 NOT 节点的数量不减少反而增加。即便如此，我们依然会无条件地执行这一转换，原因如下：
 * 1. 在 WHERE 子句中，尽可能暴露顶层的 AND/OR 结构是至关重要的；
 * 2. 移除中间层的 NOT 节点，可能让我们能够将原本无法合并的两层 AND 或 OR 结构进行扁平化合并；
 * 3. 执行此项操作的核心动机之一，是确保逻辑等价的表达式在物理层面能够被 `equal()` 函数判定为相等，
 *    因此我们应当始终应用一致的转换规则。
 */
/*
 * 对一个输入表达式取反。
 *		1. 对null取反仍然为null。
 *		2. 对于操作符，使用其反向操作符进行优化，即 NOT op => op->oprnegate
 *		3. 对于bool表达式:
 *			(NOT (AND A B)) => (OR (NOT A) (NOT B))
 *			(NOT (OR A B))	=> (AND (NOT A) (NOT B))
 *			NOT(NOT A) => A
 *			对于前两条规则，是为了把not下推到地层表达式中，以便于继续优化。
*		4. 对 is_null进行优化：
			如果原始为is_null，那么优化为is_not_null；
			如果原始为is_not_null，那么优化为is_null;
		5. 对bool test表达式进行优化：
			is_true => is_not_true,
			is_not_true => is true,
			is_false => is_not_false,
			is_not_false => is false,
			is_known => is_not_known,
			is_not_known => is_known;
		6. 对标量数组操作，也是取数组的反向操作符进行优化。
 */
Node *
negate_clause(Node *node)
{
	if (node == NULL)			/* should not happen */
		elog(ERROR, "can't negate an empty subexpression");
	switch (nodeTag(node))
	{
		case T_Const:
			{
				/*
				 *	处理常量。
				 */
				Const	   *c = (Const *) node;

				/* NOT NULL is still NULL */
				if (c->constisnull)
					return makeBoolConst(false, true);
				/* otherwise pretty easy */
				return makeBoolConst(!DatumGetBool(c->constvalue), false);
			}
			break;
		case T_OpExpr:
			{
				/*
				 * 处理操作符表达式。(NOT (< A B)) => (>= A B)
				 */

				/*
				 * Negate operator if possible: (NOT (< A B)) => (>= A B)
				 */
				OpExpr	   *opexpr = (OpExpr *) node;
				Oid			negator = get_negator(opexpr->opno);	//获得一个操作符的反向操作符。

				//用反向操作符构造一个新的节点。
				if (negator)
				{
					OpExpr	   *newopexpr = makeNode(OpExpr);

					newopexpr->opno = negator;
					newopexpr->opfuncid = InvalidOid;
					newopexpr->opresulttype = opexpr->opresulttype;
					newopexpr->opretset = opexpr->opretset;
					newopexpr->opcollid = opexpr->opcollid;
					newopexpr->inputcollid = opexpr->inputcollid;
					newopexpr->args = opexpr->args;
					newopexpr->location = opexpr->location;
					return (Node *) newopexpr;
				}
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				/*
				 * 对标量数组操作，也是取数组的反向操作符进行优化。
				 */

				/*
				 * Negate a ScalarArrayOpExpr if its operator has a negator;
				 * for example x = ANY (list) becomes x <> ALL (list)
				 */
				ScalarArrayOpExpr *saopexpr = (ScalarArrayOpExpr *) node;
				Oid			negator = get_negator(saopexpr->opno);

				if (negator)
				{
					ScalarArrayOpExpr *newopexpr = makeNode(ScalarArrayOpExpr);

					newopexpr->opno = negator;
					newopexpr->opfuncid = InvalidOid;
					newopexpr->hashfuncid = InvalidOid;
					newopexpr->negfuncid = InvalidOid;
					newopexpr->useOr = !saopexpr->useOr;
					newopexpr->inputcollid = saopexpr->inputcollid;
					newopexpr->args = saopexpr->args;
					newopexpr->location = saopexpr->location;
					return (Node *) newopexpr;
				}
			}
			break;
		case T_BoolExpr:
			{
				/*
				 * 优化bool表达式。
				 */
				BoolExpr   *expr = (BoolExpr *) node;

				switch (expr->boolop)
				{
						/*--------------------
						 * Apply DeMorgan's Laws:
						 *		(NOT (AND A B)) => (OR (NOT A) (NOT B))
						 *		(NOT (OR A B))	=> (AND (NOT A) (NOT B))
						 * i.e., swap AND for OR and negate each subclause.
						 *
						 * If the input is already AND/OR flat and has no NOT
						 * directly above AND or OR, this transformation preserves
						 * those properties.  For example, if no direct child of
						 * the given AND clause is an AND or a NOT-above-OR, then
						 * the recursive calls of negate_clause() can't return any
						 * OR clauses.  So we needn't call pull_ors() before
						 * building a new OR clause.  Similarly for the OR case.
						 *--------------------
						 */
					case AND_EXPR:
						{
							//优化规则：(NOT (AND A B)) => (OR (NOT A) (NOT B))
							List	   *nargs = NIL;
							ListCell   *lc;

							foreach(lc, expr->args)
							{
								nargs = lappend(nargs,
												negate_clause(lfirst(lc)));
							}
							return (Node *) make_orclause(nargs);
						}
						break;
					case OR_EXPR:
						{
							//优化规则：(NOT (OR A B))	=> (AND (NOT A) (NOT B))
							List	   *nargs = NIL;
							ListCell   *lc;

							foreach(lc, expr->args)
							{
								nargs = lappend(nargs,
												negate_clause(lfirst(lc)));
							}
							return (Node *) make_andclause(nargs);
						}
						break;
					case NOT_EXPR:
						//优化规则： NOT(NOT A)  => A
						/*
						 * NOT underneath NOT: they cancel.  We assume the
						 * input is already simplified, so no need to recurse.
						 */
						return (Node *) linitial(expr->args);
					default:
						elog(ERROR, "unrecognized boolop: %d",
							 (int) expr->boolop);
						break;
				}
			}
			break;
		case T_NullTest:
			{
				/*
				 * 对 is_null进行优化：
				 *		如果原始为is_null，那么优化为is_not_null；
				 *		如果原始为is_not_null，那么优化为is_null;
				 */
				NullTest   *expr = (NullTest *) node;

				/*
				 * In the rowtype case, the two flavors of NullTest are *not*
				 * logical inverses, so we can't simplify.  But it does work
				 * for scalar datatypes.
				 */
				if (!expr->argisrow)
				{
					NullTest   *newexpr = makeNode(NullTest);

					newexpr->arg = expr->arg;
					newexpr->nulltesttype = (expr->nulltesttype == IS_NULL ?
											 IS_NOT_NULL : IS_NULL);
					newexpr->argisrow = expr->argisrow;
					newexpr->location = expr->location;
					return (Node *) newexpr;
				}
			}
			break;
		case T_BooleanTest:
			{
				/*
				 * 优化bool test表达式。
				 */
				BooleanTest *expr = (BooleanTest *) node;
				BooleanTest *newexpr = makeNode(BooleanTest);

				newexpr->arg = expr->arg;
				switch (expr->booltesttype)
				{
					case IS_TRUE:
						newexpr->booltesttype = IS_NOT_TRUE;
						break;
					case IS_NOT_TRUE:
						newexpr->booltesttype = IS_TRUE;
						break;
					case IS_FALSE:
						newexpr->booltesttype = IS_NOT_FALSE;
						break;
					case IS_NOT_FALSE:
						newexpr->booltesttype = IS_FALSE;
						break;
					case IS_UNKNOWN:
						newexpr->booltesttype = IS_NOT_UNKNOWN;
						break;
					case IS_NOT_UNKNOWN:
						newexpr->booltesttype = IS_UNKNOWN;
						break;
					default:
						elog(ERROR, "unrecognized booltesttype: %d",
							 (int) expr->booltesttype);
						break;
				}
				newexpr->location = expr->location;
				return (Node *) newexpr;
			}
			break;
		default:
			/* else fall through */
			break;
	}

	/*
	 * Otherwise we don't know how to simplify this, so just tack on an
	 * explicit NOT node.
	 */
	//其他不能够或者不需要优化的情况，直接返回一个not字句，即 NOT(原节点)。
	return (Node *) make_notclause((Expr *) node);
}


/*
 * canonicalize_qual
 *	  Convert a qualification expression to the most useful form.
 *
 * This is primarily intended to be used on top-level WHERE (or JOIN/ON)
 * clauses.  It can also be used on top-level CHECK constraints, for which
 * pass is_check = true.  DO NOT call it on any expression that is not known
 * to be one or the other, as it might apply inappropriate simplifications.
 *
 * The name of this routine is a holdover from a time when it would try to
 * force the expression into canonical AND-of-ORs or OR-of-ANDs form.
 * Eventually, we recognized that that had more theoretical purity than
 * actual usefulness, and so now the transformation doesn't involve any
 * notion of reaching a canonical form.
 *
 * NOTE: we assume the input has already been through eval_const_expressions
 * and therefore possesses AND/OR flatness.  Formerly this function included
 * its own flattening logic, but that requires a useless extra pass over the
 * tree.
 *
 * Returns the modified qualification.
 */
Expr *
canonicalize_qual(Expr *qual, bool is_check)
{
	Expr	   *newqual;

	/* Quick exit for empty qual */
	if (qual == NULL)
		return NULL;

	/* This should not be invoked on quals in implicit-AND format */
	Assert(!IsA(qual, List));

	/*
	 * Pull up redundant subclauses in OR-of-AND trees.  We do this only
	 * within the top-level AND/OR structure; there's no point in looking
	 * deeper.  Also remove any NULL constants in the top-level structure.
	 */
	newqual = find_duplicate_ors(qual, is_check);

	return newqual;
}


/*
 * pull_ands
 *	  Recursively flatten nested AND clauses into a single and-clause list.
 *
 * Input is the arglist of an AND clause.
 * Returns the rebuilt arglist (note original list structure is not touched).
 */
static List *
pull_ands(List *andlist)
{
	List	   *out_list = NIL;
	ListCell   *arg;

	foreach(arg, andlist)
	{
		Node	   *subexpr = (Node *) lfirst(arg);

		if (is_andclause(subexpr))
			out_list = list_concat(out_list,
								   pull_ands(((BoolExpr *) subexpr)->args));
		else
			out_list = lappend(out_list, subexpr);
	}
	return out_list;
}

/*
 * pull_ors
 *	  Recursively flatten nested OR clauses into a single or-clause list.
 *
 * Input is the arglist of an OR clause.
 * Returns the rebuilt arglist (note original list structure is not touched).
 */
static List *
pull_ors(List *orlist)
{
	List	   *out_list = NIL;
	ListCell   *arg;

	foreach(arg, orlist)
	{
		Node	   *subexpr = (Node *) lfirst(arg);

		if (is_orclause(subexpr))
			out_list = list_concat(out_list,
								   pull_ors(((BoolExpr *) subexpr)->args));
		else
			out_list = lappend(out_list, subexpr);
	}
	return out_list;
}


/*--------------------
 * The following code attempts to apply the inverse OR distributive law:
 *		((A AND B) OR (A AND C))  =>  (A AND (B OR C))
 * That is, locate OR clauses in which every subclause contains an
 * identical term, and pull out the duplicated terms.
 *
 * This may seem like a fairly useless activity, but it turns out to be
 * applicable to many machine-generated queries, and there are also queries
 * in some of the TPC benchmarks that need it.  This was in fact almost the
 * sole useful side-effect of the old prepqual code that tried to force
 * the query into canonical AND-of-ORs form: the canonical equivalent of
 *		((A AND B) OR (A AND C))
 * is
 *		((A OR A) AND (A OR C) AND (B OR A) AND (B OR C))
 * which the code was able to simplify to
 *		(A AND (A OR C) AND (B OR A) AND (B OR C))
 * thus successfully extracting the common condition A --- but at the cost
 * of cluttering the qual with many redundant clauses.
 *--------------------
 */

/*
 * find_duplicate_ors
 *	  Given a qualification tree with the NOTs pushed down, search for
 *	  OR clauses to which the inverse OR distributive law might apply.
 *	  Only the top-level AND/OR structure is searched.
 *
 * While at it, we remove any NULL constants within the top-level AND/OR
 * structure, eg in a WHERE clause, "x OR NULL::boolean" is reduced to "x".
 * In general that would change the result, so eval_const_expressions can't
 * do it; but at top level of WHERE, we don't need to distinguish between
 * FALSE and NULL results, so it's valid to treat NULL::boolean the same
 * as FALSE and then simplify AND/OR accordingly.  Conversely, in a top-level
 * CHECK constraint, we may treat a NULL the same as TRUE.
 *
 * Returns the modified qualification.  AND/OR flatness is preserved.
 */
static Expr *
find_duplicate_ors(Expr *qual, bool is_check)
{
	if (is_orclause(qual))
	{
		List	   *orlist = NIL;
		ListCell   *temp;

		/* Recurse */
		foreach(temp, ((BoolExpr *) qual)->args)
		{
			Expr	   *arg = (Expr *) lfirst(temp);

			arg = find_duplicate_ors(arg, is_check);

			/* Get rid of any constant inputs */
			if (arg && IsA(arg, Const))
			{
				Const	   *carg = (Const *) arg;

				if (is_check)
				{
					/* Within OR in CHECK, drop constant FALSE */
					if (!carg->constisnull && !DatumGetBool(carg->constvalue))
						continue;
					/* Constant TRUE or NULL, so OR reduces to TRUE */
					return (Expr *) makeBoolConst(true, false);
				}
				else
				{
					/* Within OR in WHERE, drop constant FALSE or NULL */
					if (carg->constisnull || !DatumGetBool(carg->constvalue))
						continue;
					/* Constant TRUE, so OR reduces to TRUE */
					return arg;
				}
			}

			orlist = lappend(orlist, arg);
		}

		/* Flatten any ORs pulled up to just below here */
		orlist = pull_ors(orlist);

		/* Now we can look for duplicate ORs */
		return process_duplicate_ors(orlist);
	}
	else if (is_andclause(qual))
	{
		List	   *andlist = NIL;
		ListCell   *temp;

		/* Recurse */
		foreach(temp, ((BoolExpr *) qual)->args)
		{
			Expr	   *arg = (Expr *) lfirst(temp);

			arg = find_duplicate_ors(arg, is_check);

			/* Get rid of any constant inputs */
			if (arg && IsA(arg, Const))
			{
				Const	   *carg = (Const *) arg;

				if (is_check)
				{
					/* Within AND in CHECK, drop constant TRUE or NULL */
					if (carg->constisnull || DatumGetBool(carg->constvalue))
						continue;
					/* Constant FALSE, so AND reduces to FALSE */
					return arg;
				}
				else
				{
					/* Within AND in WHERE, drop constant TRUE */
					if (!carg->constisnull && DatumGetBool(carg->constvalue))
						continue;
					/* Constant FALSE or NULL, so AND reduces to FALSE */
					return (Expr *) makeBoolConst(false, false);
				}
			}

			andlist = lappend(andlist, arg);
		}

		/* Flatten any ANDs introduced just below here */
		andlist = pull_ands(andlist);

		/* AND of no inputs reduces to TRUE */
		if (andlist == NIL)
			return (Expr *) makeBoolConst(true, false);

		/* Single-expression AND just reduces to that expression */
		if (list_length(andlist) == 1)
			return (Expr *) linitial(andlist);

		/* Else we still need an AND node */
		return make_andclause(andlist);
	}
	else
		return qual;
}

/*
 * process_duplicate_ors
 *	  Given a list of exprs which are ORed together, try to apply
 *	  the inverse OR distributive law.
 *
 * Returns the resulting expression (could be an AND clause, an OR
 * clause, or maybe even a single subexpression).
 */
static Expr *
process_duplicate_ors(List *orlist)
{
	List	   *reference = NIL;
	int			num_subclauses = 0;
	List	   *winners;
	List	   *neworlist;
	ListCell   *temp;

	/* OR of no inputs reduces to FALSE */
	if (orlist == NIL)
		return (Expr *) makeBoolConst(false, false);

	/* Single-expression OR just reduces to that expression */
	if (list_length(orlist) == 1)
		return (Expr *) linitial(orlist);

	/*
	 * Choose the shortest AND clause as the reference list --- obviously, any
	 * subclause not in this clause isn't in all the clauses. If we find a
	 * clause that's not an AND, we can treat it as a one-element AND clause,
	 * which necessarily wins as shortest.
	 */
	foreach(temp, orlist)
	{
		Expr	   *clause = (Expr *) lfirst(temp);

		if (is_andclause(clause))
		{
			List	   *subclauses = ((BoolExpr *) clause)->args;
			int			nclauses = list_length(subclauses);

			if (reference == NIL || nclauses < num_subclauses)
			{
				reference = subclauses;
				num_subclauses = nclauses;
			}
		}
		else
		{
			reference = list_make1(clause);
			break;
		}
	}

	/*
	 * Just in case, eliminate any duplicates in the reference list.
	 */
	reference = list_union(NIL, reference);

	/*
	 * Check each element of the reference list to see if it's in all the OR
	 * clauses.  Build a new list of winning clauses.
	 */
	winners = NIL;
	foreach(temp, reference)
	{
		Expr	   *refclause = (Expr *) lfirst(temp);
		bool		win = true;
		ListCell   *temp2;

		foreach(temp2, orlist)
		{
			Expr	   *clause = (Expr *) lfirst(temp2);

			if (is_andclause(clause))
			{
				if (!list_member(((BoolExpr *) clause)->args, refclause))
				{
					win = false;
					break;
				}
			}
			else
			{
				if (!equal(refclause, clause))
				{
					win = false;
					break;
				}
			}
		}

		if (win)
			winners = lappend(winners, refclause);
	}

	/*
	 * If no winners, we can't transform the OR
	 */
	if (winners == NIL)
		return make_orclause(orlist);

	/*
	 * Generate new OR list consisting of the remaining sub-clauses.
	 *
	 * If any clause degenerates to empty, then we have a situation like (A
	 * AND B) OR (A), which can be reduced to just A --- that is, the
	 * additional conditions in other arms of the OR are irrelevant.
	 *
	 * Note that because we use list_difference, any multiple occurrences of a
	 * winning clause in an AND sub-clause will be removed automatically.
	 */
	neworlist = NIL;
	foreach(temp, orlist)
	{
		Expr	   *clause = (Expr *) lfirst(temp);

		if (is_andclause(clause))
		{
			List	   *subclauses = ((BoolExpr *) clause)->args;

			subclauses = list_difference(subclauses, winners);
			if (subclauses != NIL)
			{
				if (list_length(subclauses) == 1)
					neworlist = lappend(neworlist, linitial(subclauses));
				else
					neworlist = lappend(neworlist, make_andclause(subclauses));
			}
			else
			{
				neworlist = NIL;	/* degenerate case, see above */
				break;
			}
		}
		else
		{
			if (!list_member(winners, clause))
				neworlist = lappend(neworlist, clause);
			else
			{
				neworlist = NIL;	/* degenerate case, see above */
				break;
			}
		}
	}

	/*
	 * Append reduced OR to the winners list, if it's not degenerate, handling
	 * the special case of one element correctly (can that really happen?).
	 * Also be careful to maintain AND/OR flatness in case we pulled up a
	 * sub-sub-OR-clause.
	 */
	if (neworlist != NIL)
	{
		if (list_length(neworlist) == 1)
			winners = lappend(winners, linitial(neworlist));
		else
			winners = lappend(winners, make_orclause(pull_ors(neworlist)));
	}

	/*
	 * And return the constructed AND clause, again being wary of a single
	 * element and AND/OR flatness.
	 */
	if (list_length(winners) == 1)
		return (Expr *) linitial(winners);
	else
		return make_andclause(pull_ands(winners));
}
