-- ===================================================================
-- test shard repair functionality
-- ===================================================================

-- create a table and create its distribution metadata
CREATE TABLE customer_engagements ( id integer, created_at date, event_data text );

-- add some indexes
CREATE INDEX ON customer_engagements (id);
CREATE INDEX ON customer_engagements (created_at);
CREATE INDEX ON customer_engagements (event_data);

INSERT INTO pgs_distribution_metadata.partition (relation_id, partition_method, key)
VALUES
	('customer_engagements', 'h', 'id');

INSERT INTO pgs_distribution_metadata.shard
	(id, relation_id, storage, min_value, max_value)
VALUES
	(20, 'customer_engagements', 't', '-2147483648', '2147483647');

-- Note we are "distributing" this table on localhost and 127.0.0.1, i.e. two
-- hostnames for the same machine. This is a hack to get the pg_shard master to
-- connect back to itself (most we can hope for with installcheck). The other
-- entries are to test input parameter validation.
INSERT INTO pgs_distribution_metadata.shard_placement
	(id, node_name, node_port, shard_id, shard_state)
VALUES
	(200, 'localhost', :worker_port, 20, 1),
	(201, '127.0.0.1', :worker_port, 20, 3),
	(202, 'dummyhost', :worker_port, 20, 1),
	(203, 'otherhost', :worker_port, 20, 3);

-- first, test input checking by trying to copy into a finalized placement
SELECT master_copy_shard_placement(20, 'localhost', :worker_port, 'dummyhost', :worker_port);

-- also try to copy from an inactive placement
SELECT master_copy_shard_placement(20, 'otherhost', :worker_port, '127.0.0.1', :worker_port);

-- next, create an empty "shard" for the table
CREATE TABLE customer_engagements_20 ( LIKE customer_engagements );

-- capture its current object identifier
\o /dev/null
SELECT 'customer_engagements_20'::regclass::oid AS shardoid;
\gset
\o

-- "copy" this shard from localhost to 127.0.0.1
SELECT master_copy_shard_placement(20, 'localhost', :worker_port, '127.0.0.1', :worker_port);

-- the table was recreated, so capture the new object identifier
\o /dev/null
SELECT 'customer_engagements_20'::regclass::oid AS repairedoid;
\gset
\o

-- the recreated table should have a new oid
SELECT :shardoid != :repairedoid AS shard_recreated; 

-- now do the same test over again with a foreign table
CREATE FOREIGN TABLE remote_engagements (
	id integer,
	created_at date,
	event_data text
) SERVER fake_fdw_server;

INSERT INTO pgs_distribution_metadata.partition (relation_id, partition_method, key)
VALUES
	('remote_engagements', 'h', 'id');

INSERT INTO pgs_distribution_metadata.shard
	(id, relation_id, storage, min_value, max_value)
VALUES
	(30, 'remote_engagements', 'f', '-2147483648', '2147483647');

INSERT INTO pgs_distribution_metadata.shard_placement
	(id, node_name, node_port, shard_id, shard_state)
VALUES
	(300, 'localhost', :worker_port, 30, 1),
	(301, '127.0.0.1', :worker_port, 30, 3);

CREATE FOREIGN TABLE remote_engagements_30 (
	id integer,
	created_at date,
	event_data text
) SERVER fake_fdw_server;

-- oops! we don't support repairing shards backed by foreign tables
SELECT master_copy_shard_placement(30, 'localhost', :worker_port, '127.0.0.1', :worker_port);

-- At this point, we've tested recreating a shard's table, but haven't seen
-- whether the rows themselves are correctly copied. We'll insert a few rows
-- into our "shard" and use our hack to get the pg_shard worker to connect back
-- to itself and copy the rows back into their own shard, i.e. doubling rows.
INSERT INTO customer_engagements_20 DEFAULT VALUES;

-- call the copy UDF directly to just copy the rows without recreating table
SELECT worker_copy_shard_placement('customer_engagements_20', 'localhost', :worker_port);

-- should expect twice as many rows as we put in
SELECT COUNT(*) FROM customer_engagements_20;
