from test_tools import logger, Account, World, Asset, BlockLog
import time
import sqlalchemy


FIRST_BLOCK_TIMESTAMP = int(time.time()) - 200 * 3
BLOCKS_IN_FORK = 10
BACK_FROM_FORK_BLOCKS = 10
FORKS = 500


def test_blocks_table_after_fork(world_with_witnesses):
    world = world_with_witnesses
    #GIVEN
    beta_net = world.network('Beta')
    node_under_test = beta_net.node('NodeUnderTest')

    engine = sqlalchemy.create_engine('postgresql://myuser:mypassword@localhost/haf_block_log', echo=False)
    database_under_test = engine.connect()

    fork_block = node_under_test.api.database.get_dynamic_global_properties()["result"]["head_block_number"]

    for fork_number in range(FORKS):
        # WHEN
        logger.info(f'fork number {fork_number}')
        fork_block = node_under_test.api.database.get_dynamic_global_properties()["result"]["head_block_number"]
        logger.info(f'making fork at block {fork_block}')
        node_under_test.wait_for_block_with_number(fork_block)
        make_fork(world, fork_number)

        # THEN
        irreversible_block_num = node_under_test.api.database.get_dynamic_global_properties()["result"]["last_irreversible_block_num"]
        logger.info(f'last_irreversible_block_num: {irreversible_block_num}')
        result = database_under_test.execute('select * from hive.blocks order by num;')
        blocks = set()
        for row in result:
            blocks.add(row['num'])
        logger.info(f'blocks: {blocks}')

        head_block_num = node_under_test.api.database.get_dynamic_global_properties()["result"]["head_block_number"]
        logger.info(f'head_block_number: {head_block_num}')
        result = database_under_test.execute('select * from hive.blocks_reversible order by num;')
        blocks_reversible = set()
        for row in result:
            blocks_reversible.add(row['num'])
        logger.info(f'blocks_reversible: {blocks_reversible}')

        # assert table hive.blocks contains blocks 1..irreversible_block_num
        for i in range(1, irreversible_block_num+1):
            assert i in blocks
        # assert table hive.blocks contains blocks irreversible_block_num+1..head_block_number
        for i in range(irreversible_block_num+1, head_block_num+1):
            assert i in blocks_reversible


def make_fork(world, fork_number):
    alpha_net = world.network('Alpha')
    beta_net = world.network('Beta')
    alpha_witness_node = alpha_net.node('WitnessNode0')
    beta_witness_node = beta_net.node('WitnessNode0')

    wallet = beta_witness_node.attach_wallet()
    fork_block = beta_witness_node.api.database.get_dynamic_global_properties()["result"]["head_block_number"]
    head_block = fork_block
    alpha_net.disconnect_from(beta_net)

    with wallet.in_single_transaction():
        name = "dummy-acnt-" + str(fork_number)
        wallet.api.create_account('initminer', name, '')
        logger.info('create_account created...')
    logger.info('create_account sent...')

    alpha_witness_node.wait_for_block_with_number(head_block + BLOCKS_IN_FORK)
    beta_witness_node.wait_for_block_with_number(head_block + BLOCKS_IN_FORK)
    alpha_net.connect_with(beta_net)
    alpha_witness_node.wait_for_block_with_number(head_block + BLOCKS_IN_FORK + BACK_FROM_FORK_BLOCKS)
    beta_witness_node.wait_for_block_with_number(head_block + BLOCKS_IN_FORK + BACK_FROM_FORK_BLOCKS)
