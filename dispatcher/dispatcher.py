#!/daq_common/miniconda3/bin/python3
import configparser
import argparse
import threading
import datetime
import os
import daqnt
import json
from pymongo import MongoClient
from urllib.parse import quote_plus

from MongoConnect import MongoConnect
from DAQController import DAQController


def main():

    # Parse command line
    parser = argparse.ArgumentParser(description='Manage the DAQ')
    parser.add_argument('--config', type=str, help='Path to your configuration file',
            default='config.ini')
    parser.add_argument('--log', type=str, help='Logging level', default='DEBUG',
            choices=['DEBUG','INFO','WARNING','ERROR','CRITICAL'])
    parser.add_argument('--test', action='store_true', help='Are you testing?')
    args = parser.parse_args()
    config = configparser.ConfigParser()
    config.read(args.config)
    config = config['DEFAULT' if not args.test else "TESTING"]
    config['MasterDAQConfig'] = json.loads(config['MasterDAQConfig'])
    control_mc = daqnt.get_client('daq')
    runs_mc = daqnt.get_client('runs')
    logger = daqnt.get_daq_logger(config['LogName'], level=args.log, mc=control_mc)
    vme_config = json.loads(config['VMEConfig'])

    # Declare necessary classes
    sh = daqnt.SignalHandler()
    Hypervisor = daqnt.Hypervisor(control_mc[config['ControlDatabaseName']], logger,
            config['MasterDAQConfig']['tpc'], vme_config, sh=sh, testing=args.test)
    MongoConnector = MongoConnect(config, logger, control_mc, runs_mc, Hypervisor, args.test)
    DAQControl = DAQController(config, MongoConnector, logger, Hypervisor)

    sleep_period = int(config['PollFrequency'])

    logger.info('Dispatcher starting up')

    while(sh.event.is_set() == False):
        sh.event.wait(sleep_period)
        # Get most recent check-in from all connected hosts
        if MongoConnector.get_update():
            continue
        latest_status = MongoConnector.latest_status

        # Get most recent goal state from database. Users will update this from the website.
        if (goal_state := MongoConnector.get_wanted_state()) is None:
            continue

        # Print an update
        for detector in latest_status.keys():
            state = 'ACTIVE' if goal_state[detector]['active'] == 'true' else 'INACTIVE'
            msg = (f'The {detector} should be {state} and is '
                    f'{latest_status[detector]["status"].name}')
            if latest_status[detector]['number'] != -1:
                msg += f' ({latest_status[detector]["number"]})'
            logger.debug(msg)

        # Decision time. Are we actually in our goal state? If not what should we do?
        DAQControl.solve_problem(latest_status, goal_state)

        # Time to report back
        MongoConnector.update_aggregate_status()

    MongoConnector.quit()
    return


if __name__ == '__main__':
    main()
