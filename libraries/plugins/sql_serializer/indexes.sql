CREATE INDEX IF NOT EXISTS hive_blocks_num_idx ON hive_blocks (num);

CREATE INDEX IF NOT EXISTS hive_transactions_block_num_trx_in_block_idx ON hive_transactions (block_num, trx_in_block);

CREATE INDEX IF NOT EXISTS hive_operations_block_num_type_idx ON hive_operations (block_num, op_type_id);