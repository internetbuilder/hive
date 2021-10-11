import unittest
from sqlalchemy.orm.exc import NoResultFound
from sqlalchemy.orm.exc import MultipleResultsFound
import pytest
from pathlib import Path

from test_tools import logger


MASSIVE_SYNC_BLOCK_NUM = 100


@pytest.mark.parametrize("world_with_witnesses_and_database", [Path().resolve()], indirect=True)
def test_undo_operations(world_with_witnesses_and_database):
    logger.info(f'Start test_event_massive_sync')

    #GIVEN
    world, session, Base = world_with_witnesses_and_database
    node_under_test = world.network('Beta').node('NodeUnderTest')

    events_queue = Base.classes.events_queue

    # WHEN
    # TODO get_p2p_endpoint is workaround to check if replay is finished
    node_under_test.get_p2p_endpoint()
    previous_irreversible = node_under_test.api.database.get_dynamic_global_properties()["result"]["last_irreversible_block_num"]
    previous_head = node_under_test.api.database.get_dynamic_global_properties()["result"]["head_block_number"]

    # THEN
    case = unittest.TestCase()
    logger.info(f'Checking that event NEW_IRREVERSIBLE and NEW_BLOCK appear in database in correct order')
    for i in range(30):
        node_under_test.wait_number_of_blocks(1)
        irreversible_block_num = node_under_test.api.database.get_dynamic_global_properties()["result"]["last_irreversible_block_num"]
        head_block_number = node_under_test.api.database.get_dynamic_global_properties()["result"]["head_block_number"]

        if irreversible_block_num > previous_irreversible:
            new_irreversible_events = session.query(events_queue).\
                filter(events_queue.event == 'NEW_IRREVERSIBLE').\
                filter(events_queue.block_num == irreversible_block_num).\
                one()
            new_block_events = session.query(events_queue).\
                filter(events_queue.event == 'NEW_BLOCK').\
                all()
            assert new_block_events == []
            logger.info(f'head_block_number {head_block_number}')
            logger.info(f'irreversible_block_num {irreversible_block_num}')

            previous_irreversible = irreversible_block_num
            previous_head = head_block_number

        else:
            new_irreversible_events = session.query(events_queue).\
                filter(events_queue.event == 'NEW_IRREVERSIBLE').\
                filter(events_queue.block_num == irreversible_block_num).\
                one()
            new_block_events = session.query(events_queue).\
                filter(events_queue.event == 'NEW_BLOCK').\
                filter(events_queue.block_num == head_block_number).\
                all()
            new_block_nums = [event.block_num for event in new_block_events]
            logger.info(f'head_block_number {head_block_number}')
            logger.info(f'irreversible_block_num {irreversible_block_num}')
            logger.info('expected' + ' '*20 + str(range(previous_head+1, head_block_number+1)))
            logger.info('actual  ' + ' '*20 + str(new_block_nums))
            # case.assertCountEqual(new_block_nums, range(previous_head+1, head_block_number+1))




        # logger.info(f"STARTING ITERATION {i}")
        # for entry in res:
        #     logger.info(f'event {entry.event} block_num {entry.block_num}')


