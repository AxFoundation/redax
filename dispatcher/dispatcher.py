#!/daq_common2/miniconda3/bin/python3
import configparser
import argparse
import os
import daqnt
import json
import subprocess
import time

from MongoConnect import MongoConnect
from DAQController import DAQController
from hypervisor import Hypervisor


def setup():
    # Parse command line
    err = None
    try:
        commit = subprocess.run('git log -n 1 --pretty=oneline'.split(),
                                capture_output=True
                                ).stdout.decode().split(' ')[0]
    except Exception as e:
        err = f'Couldn\'t get commit hash: {type(e)}, {e}'
        commit = 'unknown'

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
    daq_config = json.loads(config['MasterDAQConfig'])
    control_mc = daqnt.get_client('daq')
    runs_mc = daqnt.get_client('runs')
    logger = daqnt.get_daq_logger(config['LogName'], level=args.log, mc=control_mc, opening_message=f'Commit {commit}')
    vme_config = json.loads(config['VMEConfig'])
    if err is not None:
        logger.error(err)

    SlackBot = daqnt.DaqntBot(os.environ['SLACK_KEY'])
    while True:
        try:
            main(config, control_mc, logger, daq_config, vme_config, SlackBot, runs_mc, args)
        except (KeyboardInterrupt, SystemExit):
            raise
        except Exception as fatal_error:
            logger.debug(fatal_error, exc_info=True)
            logger.error(f'Fatal warning:\tran into {fatal_error}. Try '
                         f'logging error and restart the dispatcher')
            logger.critical(f'Fatal dispatcher exception: {fatal_error}')
            SlackBot.send_message(
                f'Dispatcher just died ({fatal_error}), this is very bad. '
                'We\'re going to try a reboot but please alert the DAQ-group.',
                add_tags='ALL')
            time.sleep(60)
            logger.warning('Restarting main loop')


def main(config, control_mc, logger, daq_config, vme_config, SlackBot, runs_mc, args):
    sh = daqnt.SignalHandler()
    sleep_period = int(config['PollFrequency'])

    # Declare necessary classes
    hv = Hypervisor(
        control_mc[config['ControlDatabaseName']],
        logger,
        daq_config,
        vme_config,
        control_inputs=config['ControlKeys'].split(),
        testing=args.test,
        slackbot=SlackBot,
    )
    gps_start = config['StartWithGPS'].split()
    gps_period = int(config['GPSPeriod')
    mc = MongoConnect(config, daq_config, logger, control_mc, runs_mc, hv, args.test, sleep_period, gps_start, gps_period)
    dc = DAQController(config, daq_config, mc, logger, hv, sleep_period, gps_start)

    # connect the triangle
    hv.mongo_connect = mc
    hv.daq_controller = dc


    last_loop_dt = 0

    while sh.event.is_set() == False:
        sh.event.wait(max(0, sleep_period - last_loop_dt))
        t_start = time.time()
        # Get most recent goal state from database. Users will update this from the website.
        if (goal_state := mc.get_wanted_state()) is None:
            continue
        # Get the Super-Detector configuration
        current_config = mc.get_super_detector()
        # Get most recent check-in from all connected hosts
        if (latest_status := mc.get_update(current_config)) is None:
            continue

        # Print an update
        for detector in latest_status.keys():
            state = 'ACTIVE' if goal_state[detector]['active'] == 'true' else 'INACTIVE'
            msg = (f'The {detector} should be {state} and is '
                   f'{latest_status[detector]["status"].name}')
            if latest_status[detector]['number'] != -1:
                msg += f' ({latest_status[detector]["number"]})'
            logger.debug(msg)
        msg = (f"Linking: tpc-mv: {mc.is_linked('tpc', 'muon_veto')}, "
               f"tpc-nv: {mc.is_linked('tpc', 'neutron_veto')}, "
               f"mv-nv: {mc.is_linked('muon_veto', 'neutron_veto')}")
        logger.debug(msg)

        # Decision time. Are we actually in our goal state? If not what should we do?
        dc.solve_problem(latest_status, goal_state)

        mc.backup_aggstat()

        last_loop_dt = time.time() - t_start

    mc.quit()
    return


if __name__ == '__main__':
    setup()
