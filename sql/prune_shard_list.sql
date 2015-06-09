-- ===================================================================
-- create test functions
-- ===================================================================

CREATE FUNCTION prune_using_no_values(regclass)
	RETURNS text[]
	AS 'pg_shard'
	LANGUAGE C STRICT;

CREATE FUNCTION prune_using_single_value(regclass, text)
	RETURNS text[]
	AS 'pg_shard'
	LANGUAGE C;

CREATE FUNCTION prune_using_either_value(regclass, text, text)
	RETURNS text[]
	AS 'pg_shard'
	LANGUAGE C STRICT;

CREATE FUNCTION prune_using_both_values(regclass, text, text)
	RETURNS text[]
	AS 'pg_shard'
	LANGUAGE C STRICT;

CREATE FUNCTION debug_equality_expression(regclass)
	RETURNS cstring
	AS 'pg_shard'
	LANGUAGE C STRICT;

-- ===================================================================
-- test shard pruning functionality
-- ===================================================================

-- create distributed table metadata to observe shard pruning
CREATE TABLE pruning ( species text, last_pruned date, plant_id integer );

INSERT INTO pgs_distribution_metadata.partition (relation_id, partition_method, key)
VALUES
	('pruning', 'h', 'species');

INSERT INTO pgs_distribution_metadata.shard
	(id, relation_id, storage, min_value, max_value)
VALUES
	(10, 'pruning', 't', '-2147483648', '-1073741826'),
	(11, 'pruning', 't', '-1073741825', '-3'),
	(12, 'pruning', 't', '-2', '1073741820'),
	(13, 'pruning', 't', '1073741821', '2147483647');

-- with no values, expect all shards
SELECT prune_using_no_values('pruning');

-- with a single value, expect a single shard
SELECT prune_using_single_value('pruning', 'tomato');

-- the above is true even if that value is null
SELECT prune_using_single_value('pruning', NULL);

-- build an OR clause and expect more than one sahrd
SELECT prune_using_either_value('pruning', 'tomato', 'petunia');

-- an AND clause with incompatible values returns no shards
SELECT prune_using_both_values('pruning', 'tomato', 'petunia');

-- but if both values are on the same shard, should get back that shard
SELECT prune_using_both_values('pruning', 'tomato', 'rose');

-- unit test of the equality expression generation code
SELECT debug_equality_expression('pruning');
