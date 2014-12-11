/*-------------------------------------------------------------------------
 *
 * test/create_shards.c
 *
 * This file contains functions to exercise shard creation functionality
 * within pg_shard.
 *
 * Copyright (c) 2014, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "c.h"
#include "fmgr.h"
#include "postgres_ext.h"

#include "distribution_metadata.h"
#include "prune_shard_list.h"
#include "test/test_helper_functions.h" /* IWYU pragma: keep */

#include <string.h>

#include "access/skey.h"
#include "catalog/pg_type.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "nodes/nodes.h"
#include "optimizer/clauses.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/palloc.h"


/* local function forward declarations */
static Expr * MakeTextPartitionExpression(Oid distributedTableId, text *value);
static ArrayType * PrunedShardIdsForTable(Oid distributedTableId, List *whereClauseList);
static ArrayType * DatumArrayToArrayType(Datum *datumArray, int datumCount,
										 Oid datumTypeId);


/* declarations for dynamic loading */
PG_FUNCTION_INFO_V1(prune_using_no_values);
PG_FUNCTION_INFO_V1(prune_using_single_value);
PG_FUNCTION_INFO_V1(prune_using_either_value);
PG_FUNCTION_INFO_V1(prune_using_both_values);
PG_FUNCTION_INFO_V1(debug_equality_expression);


Datum
prune_using_no_values(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = PG_GETARG_OID(0);
	List *whereClauseList = NIL;
	ArrayType *shardIdArrayType = PrunedShardIdsForTable(distributedTableId,
	                                                     whereClauseList);

	PG_RETURN_ARRAYTYPE_P(shardIdArrayType);
}

Datum
prune_using_single_value(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = PG_GETARG_OID(0);
	text *value = (PG_ARGISNULL(1)) ? NULL : PG_GETARG_TEXT_P(1);
	Expr *equalityExpr = MakeTextPartitionExpression(distributedTableId, value);
	List *whereClauseList = list_make1(equalityExpr);
	ArrayType *shardIdArrayType = PrunedShardIdsForTable(distributedTableId,
	                                                     whereClauseList);

	PG_RETURN_ARRAYTYPE_P(shardIdArrayType);
}


Datum
prune_using_either_value(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = PG_GETARG_OID(0);
	text *firstValue = PG_GETARG_TEXT_P(1);
	text *secondValue = PG_GETARG_TEXT_P(2);
	Expr *firstQual = MakeTextPartitionExpression(distributedTableId, firstValue);
	Expr *secondQual = MakeTextPartitionExpression(distributedTableId, secondValue);
	Expr *orClause = make_orclause(list_make2(firstQual, secondQual));
	List *whereClauseList = list_make1(orClause);
	ArrayType *shardIdArrayType = PrunedShardIdsForTable(distributedTableId,
	                                                     whereClauseList);

	PG_RETURN_ARRAYTYPE_P(shardIdArrayType);
}


Datum
prune_using_both_values(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = PG_GETARG_OID(0);
	text *firstValue = PG_GETARG_TEXT_P(1);
	text *secondValue = PG_GETARG_TEXT_P(2);
	Expr *firstQual = MakeTextPartitionExpression(distributedTableId, firstValue);
	Expr *secondQual = MakeTextPartitionExpression(distributedTableId, secondValue);

	List *whereClauseList = list_make2(firstQual, secondQual);
	ArrayType *shardIdArrayType = PrunedShardIdsForTable(distributedTableId,
	                                                     whereClauseList);

	PG_RETURN_ARRAYTYPE_P(shardIdArrayType);
}


Datum
debug_equality_expression(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = PG_GETARG_OID(0);
	Var *partitionColumn = PartitionColumn(distributedTableId);
	OpExpr *equalityExpression = MakeOpExpression(partitionColumn, BTEqualStrategyNumber);

	PG_RETURN_CSTRING(nodeToString(equalityExpression));
}

static Expr *
MakeTextPartitionExpression(Oid distributedTableId, text *value)
{
	Var *partitionColumn = PartitionColumn(distributedTableId);
	Expr *partitionExpression = NULL;

	if (value != NULL)
	{
		OpExpr *equalityExpr = MakeOpExpression(partitionColumn, BTEqualStrategyNumber);
		Const *rightConst = (Const *) get_rightop((Expr *) equalityExpr);

		rightConst->constvalue = (Datum) value;
		rightConst->constisnull = false;
		rightConst->constbyval = false;

		partitionExpression = (Expr *) equalityExpr;
	}
	else
	{
		NullTest *nullTest = makeNode(NullTest);
		nullTest->arg = (Expr *) partitionColumn;
		nullTest->nulltesttype = IS_NULL;

		partitionExpression = (Expr *) nullTest;
	}

	return partitionExpression;
}


static ArrayType *
PrunedShardIdsForTable(Oid distributedTableId, List *whereClauseList)
{
	ArrayType *shardIdArrayType = NULL;
	ListCell *shardCell = NULL;
	int shardIdIndex = 0;
	Oid shardIdTypeId = INT8OID;

	List *shardList = LoadShardIntervalList(distributedTableId);
	int shardIdCount = -1;
	Datum *shardIdDatumArray = NULL;

	shardList = PruneShardList(distributedTableId, whereClauseList, shardList);

	shardIdCount = list_length(shardList);
	shardIdDatumArray = palloc0(shardIdCount * sizeof(Datum));

	foreach(shardCell, shardList)
	{
		ShardInterval *shardId = (ShardInterval *) lfirst(shardCell);
		Datum shardIdDatum = Int64GetDatum(shardId->id);

		shardIdDatumArray[shardIdIndex] = shardIdDatum;
		shardIdIndex++;
	}

	shardIdArrayType = DatumArrayToArrayType(shardIdDatumArray, shardIdCount,
											 shardIdTypeId);

	return shardIdArrayType;
}


/*
 * DatumArrayToArrayType converts the provided Datum array (of the specified
 * length and type) into an ArrayType suitable for returning from a UDF.
 */
static ArrayType *
DatumArrayToArrayType(Datum *datumArray, int datumCount, Oid datumTypeId)
{
	ArrayType *arrayObject = NULL;
	int16 typeLength = 0;
	bool typeByValue = false;
	char typeAlignment = 0;

	get_typlenbyvalalign(datumTypeId, &typeLength, &typeByValue, &typeAlignment);
	arrayObject = construct_array(datumArray, datumCount, datumTypeId,
								  typeLength, typeByValue, typeAlignment);

	return arrayObject;
}
