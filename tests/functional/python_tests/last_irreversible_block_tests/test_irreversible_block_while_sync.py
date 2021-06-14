from test_tools import logger, World


def test_irreversible_block_while_sync():
    with World() as world:
        net = world.create_network()
        init_node = net.create_init_node()
        api_node = net.create_api_node()

        logger.info('Running network, waiting for live sync...')
        net.run()

        logger.info('Waiting for block number 23...')
        init_node.wait_for_block_with_number(23)

        # Test api_node will enter live sync after restart
        logger.info("Restarting api node...")
        api_node.close()
        api_node.run(timeout=90)