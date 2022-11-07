import datetime
import json
import enum
import pytz
from daqnt import DAQ_STATUS


def now():
    return datetime.datetime.now(pytz.utc)

class DAQController():
    """
    DAQ Controller Brain Class
    D. Coderre, 12. Mar. 2019
    D. Masson, 06 Apr 2020
    S. di Pede, 17 Mar 2021
    V. D'Andrea, Oct 2022

    Brief: This code handles the logic of what the dispatcher does when. It takes in 
    aggregated status updates and commands from the mongo connector and decides if
    any action needs to be taken to get the DAQ into the target state. It also handles the
    resetting of runs (the ~hourly stop/start) during normal operations.
    """

    def __init__(self, config, daq_config, mongo_connector, logger, hypervisor):

        self.mongo = mongo_connector
        self.hypervisor = hypervisor
        self.goal_state = {}
        self.latest_status = {}

        # Timeouts. There are a few things that we want to wait for that might take time.
        # The keys for these dicts will be detector identifiers.
        detectors = list(daq_config.keys()) # physical detectors
        self.last_command = {}
        for k in ['arm', 'start', 'stop']:
            self.last_command[k] = {}
            for d in detectors:
                self.last_command[k][d] = now()
        self.error_stop_count = {d: 0 for d in detectors}
        self.max_arm_cycles = int(config['MaxArmCycles'])
        self.missed_arm_cycles = {k: 0 for k in detectors}

        # Timeout properties come from config
        self.timeouts = {
                k.lower() : int(config['%sCommandTimeout' % k])
                for k in ['Arm','Start','Stop']}
        self.stop_retries = int(config['RetryReset'])

        self.hv_nuclear_timeout = int(config['HypervisorNuclearTimeout'])
        self.last_nuke = now()

        self.logger = logger
        self.time_between_commands = int(config['TimeBetweenCommands'])
        self.can_force_stop = {k:True for k in detectors}

        self.one_detector_arming = False
        self.start_cmd_delay = float(config['StartCmdDelay'])
        self.stop_cmd_delay = float(config['StopCmdDelay'])

    def solve_problem(self, latest_status, goal_state):
        """
        This is sort of the whole thing that all the other code is supporting
        We get the status from the DAQ and the command from the user
        Then one of three things can happen:
             1) The status agrees with the command. We're in the goal state and happy.
             2) The status differs from the command. We issue the necessary commands
                to put the system into the goal state
             3) The status and goal are irreconcilable. We complain with an error
                because we can't find any way to put the system into the goal state.
                This could be because a key component is either in error or not present
                at all. Or it could be because we are trying to, for example, start a calibration
                run in the neutron veto but it is already running a combined run and
                therefore unavailable. The frontend should prevent many of these cases though.

        The way that works is this:
        A) the detector should be INACTIVE (i.e., IDLE), we stop the detector 
            if the status is in one of the active states
        B) the detector should be ACTIVE (i.e, RUNNING), we issue the necessary 
            commands to put the system in the RUNNING status
        C) we deal separately with the ERROR and TIMEOUT statuses, as in the 
            first time we need to promptly stop the detector, and in the second 
            case we need to handle the timeouts.
        """
        # cache these so other functions can see them
        self.goal_state = goal_state
        self.latest_status = latest_status
        self.one_detector_arming = False

        for logical in latest_status.keys():
            det = list(latest_status[logical]['detectors'].keys())[0]
            if latest_status[logical]['status'] == DAQ_STATUS.IDLE:
                self.can_force_stop[det] = True
                self.error_stop_count[det] = 0
            if latest_status[logical]['status'] in [DAQ_STATUS.ARMING, DAQ_STATUS.ARMED]:
                self.one_detector_arming = True

        active_states = [DAQ_STATUS.RUNNING, DAQ_STATUS.ARMED, DAQ_STATUS.ARMING, DAQ_STATUS.UNKNOWN]
        
        for logical in latest_status.keys():
            # Take the first physical detector, only one is needed to retrieve the goal status
            det = list(latest_status[logical]['detectors'].keys())[0]
            # The detector should be INACTIVE
            if goal_state[det]['active'] == 'false':
                # The detector is not in IDLE, ERROR or TIMEOUT: it needs to be stopped
                if latest_status[logical]['status'] in active_states:
                    # Check before if the status is UNKNOWN and it is maybe timing out
                    if latest_status[logical]['status'] == DAQ_STATUS.UNKNOWN:
                        self.logger.info(f"The status of {logical} is unknown, check timeouts")
                        self.check_timeouts(logical)
                    # Otherwise stop the detector
                    else:
                        self.logger.info(f"Sending stop command to {logical}")
                        self.stop_detector_gently(logical)
                # Deal separately with the TIMEOUT and ERROR statuses, by stopping the detector if needed
                elif latest_status[logical]['status'] == DAQ_STATUS.TIMEOUT:
                    self.logger.info(f"The {logical} is in timeout, check timeouts")
                    self.handle_timeout(logical)

                elif latest_status[logical]['status'] == DAQ_STATUS.ERROR:
                   self.logger.info(f"The {logical} has error, sending stop command")
                   self.control_detector(logical, 'stop', force=self.can_force_stop[det])
                   self.can_force_stop[det] = False
                else:
                    # the only remaining option is 'idle', which is fine
                    pass

            # The detector should be ACTIVE (RUNNING)
            else: #goal_state[det]['active'] == 'true':
                if latest_status[logical]['status'] == DAQ_STATUS.RUNNING:
                    self.logger.info(f"The {logical} is running")
                    self.check_run_turnover(logical)
                    # TODO does this work properly?
                    if latest_status[logical]['mode'] != goal_state[det]['mode']:
                        self.control_detector(logical, 'stop')
                # ARMED, start the run
                elif latest_status[logical]['status'] == DAQ_STATUS.ARMED:
                    self.logger.info(f"The {logical} is armed, sending start command")
                    self.control_detector(logical,'start')
                # ARMING, check if it is timing out
                elif latest_status[logical]['status'] == DAQ_STATUS.ARMING:
                    self.logger.info(f"The {logical} is arming, check timeouts")
                    self.logger.debug(f"Checking the {logical} timeouts")
                    self.check_timeouts(logical)
                # UNKNOWN, check if it is timing out
                elif latest_status[logical]['status'] == DAQ_STATUS.UNKNOWN:
                    self.logger.info(f"The status of {logical} is unknown, check timeouts")
                    self.logger.debug(f"Checking the {logical} timeouts")
                    self.check_timeouts(logical)
                # Maybe the detector is IDLE, we should arm a run
                elif latest_status[logical]['status'] == DAQ_STATUS.IDLE:
                    self.logger.info(f"The {logical} is idle, sending arm command")
                    self.control_detector(logical, 'arm')
                # Deal separately with the TIMEOUT and ERROR statuses, by stopping the detector if needed
                elif latest_status[logical]['status'] == DAQ_STATUS.TIMEOUT:
                    self.logger.info(f"The {logical} is in timeout, check timeouts")
                    self.logger.debug("Checking %s timeouts", logical)
                    self.handle_timeout(logical)

                elif latest_status[logical]['status'] == DAQ_STATUS.ERROR:
                    self.logger.info(f"The {logical} has error, sending stop command")
                    self.control_detector(logical,'stop', force=self.can_force_stop[det])
                    self.can_force_stop[det] = False
                else:
                    # shouldn't be able to get here
                    pass

        return

    def handle_timeout(self, logical):
        """
        Detector already in the TIMEOUT status are directly stopped.
        """
        det = list(latest_status[logical]['detectors'].keys())[0]
        self.control_detector(logical, 'stop', force = self.can_force_stop[det])
        self.can_force_stop[det] = False
        self.check_timeouts(logical)
        return

    def stop_detector_gently(self, logical):
        """
        Stops the detector, unless we're told to wait for the current
        run to end
        """
        det = list(latest_status[logical]['detectors'].keys())[0]
        if (
                # Running normally (not arming, error, timeout, etc)
                self.latest_status[logical]['status'] == DAQ_STATUS.RUNNING and
                # We were asked to wait for the current run to stop
                self.goal_state[det].get('softstop', 'false') == 'true'):
            self.check_run_turnover(logical)
        else:
            self.control_detector(logical,'stop')

    def control_detector(self, logical, command, force = False):
        """
        Issues the command to the detector if allowed by the timeout.
        Returns 0 if a command was issued and 1 otherwise
        """
        time_now = now()
        det = list(self.latest_status[logical]['detectors'].keys())[0]
        try:
            dt = (time_now - self.last_command[command][det]).total_seconds()
        except (KeyError, TypeError):
            dt = 2*self.timeouts[command]

        # make sure we don't rush things
        if command == 'start':
            dt_last = (time_now - self.last_command['arm'][det]).total_seconds()
        elif command == 'arm':
            dt_last = (time_now - self.last_command['stop'][det]).total_seconds()
        else:
            dt_last = self.time_between_commands*2

        if (dt > self.timeouts[command] and dt_last > self.time_between_commands) or force:
            ls = self.latest_status
            gs = self.goal_state
            if command == 'arm':
                if self.one_detector_arming:
                    self.logger.info('Another detector already arming, can\'t arm %s' % logical)
                    # this leads to run number overlaps
                    return 1
                readers, cc = self.mongo.get_hosts_for_mode(gs[det]['mode'])
                hosts = (cc, readers)
                delay = 0
                self.one_detector_arming = True
            elif command == 'start':
                readers, cc = self.mongo.get_hosts_for_mode(ls[logical]['mode'])
                hosts = (readers, cc)
                delay = self.start_cmd_delay
                #Reset arming timeout counter
                self.missed_arm_cycles[det] = 0
            else: # stop
                readers, cc = self.mongo.get_hosts_for_mode(ls[logical]['mode'], detector)
                hosts = (cc, readers)
                if force or ls[logical]['status'] not in [DAQ_STATUS.RUNNING]:
                    delay = 0
                else:
                    delay = self.stop_cmd_delay
            self.logger.debug(f'Sending {command.upper()} to {logical}')
            if self.mongo.send_command(command, hosts, gs[det]['user'],
                                       logical, gs[det]['mode'], delay, force):
                # failed
                return 1
            self.last_command[command][det] = time_now
            if command == 'start' and self.mongo.insert_run_doc(logical):
                # db having a moment
                return 0
            if (command == 'stop' and ls[logical]['number'] != -1 and
                    self.mongo.set_stop_time(ls[logical]['number'], logical, force)):
                # db having a moment
                return 0

        else:
            self.logger.debug('Can\'t send %s to %s, timeout at %i/%i' % (
                command, logical, dt, self.timeouts[command]))
            return 1
        return 0

    def check_timeouts(self, logical, command=None):
        """ 
        This one is invoked if we think we need to change states. Either a stop command needs
        to be sent, or we've detected an anomaly and want to decide what to do. 
        Basically this function decides:
          - We are not in any timeouts: send the normal stop command
          - We are waiting for something: do nothing
          - We were waiting for something but it took too long: attempt reset
        """

        time_now = now()
        det = list(self.latest_status[logical]['detectors'].keys())[0]
        #How often we have been timing out?
        if self.missed_arm_cycles[det] > self.max_arm_cycles and det == 'tpc':
            if (dt := (now()-self.last_nuke).total_seconds()) > self.hv_nuclear_timeout:
                self.logger.critical('There\'s only one way to be sure')
                self.control_detector(logical,'stop', force=True)
                if self.hypervisor.tactical_nuclear_option(self.mongo.is_linked_mode()):
                    self.last_nuke = now()
            else:
                #self.control_detector(detector=detector, command='stop')
                self.logger.debug(f'Nuclear timeout at {int(dt)}/{self.hv_nuclear_timeout}')

        if command is None: # not specified, we figure out it here
            command_times = [(cmd,doc[det]) for cmd,doc in self.last_command.items()]
            command = sorted(command_times, key=lambda x : x[1])[-1][0]
            self.logger.debug(f'Most recent command for {logical} is {command}')
        else:
            self.logger.debug(f'Checking {command} timeout for {logical}')

        dt = (time_now - self.last_command[command][det]).total_seconds()

        local_timeouts = dict(self.timeouts.items())
        local_timeouts['stop'] = self.timeouts['stop']*(self.error_stop_count[det]+1)

        if dt > local_timeouts[command]:
            # timing out, maybe send stop?
            if command == 'stop':
                if self.error_stop_count[det] >= self.stop_retries:
                    # failed too many times, issue error
                    self.mongo.log_error(
                                        ("Dispatcher control loop detects a timeout that STOP " +
                                         "can't solve"),
                                        'ERROR',
                                        "STOP_TIMEOUT")
                    # also invoke the nuclear option
                    if det == 'tpc':
                        if (dt := (now()-self.last_nuke).total_seconds()) > self.hv_nuclear_timeout:
                            self.control_detector(logical,'stop', force=True)
                            self.logger.critical('There\'s only one way to be sure')
                            if self.hypervisor.tactical_nuclear_option(self.mongo.is_linked_mode()):
                                self.last_nuke = now()
                        else:
                            self.control_detector(logical, 'stop')
                            self.logger.debug(f'Nuclear timeout at {int(dt)}/{self.hv_nuclear_timeout}')
                    self.error_stop_count[det] = 0
                else:
                    self.control_detector(logical, 'stop')
                    self.logger.debug(f'Working on a stop counter for {logical}')
                    self.error_stop_count[det] += 1
            else:
                self.mongo.log_error(
                        ('%s took more than %i seconds to %s, indicating a possible timeout or error' %
                            (logical, self.timeouts[command], command)),
                        'ERROR',
                        '%s_TIMEOUT' % command.upper())
                #Keep track of how often the arming sequence times out
                if self.control_detector(logical, 'stop') == 0:
                    # only increment the counter if we actually issued a STOP
                    self.missed_arm_cycles[det] += 1
                    self.logger.info(f'{logical} missed {self.missed_arm_cycles[det]} arm cycles')
                else:
                    self.logger.debug(f'{logical} didn\'t actually get a command, no arm cycler increment')
        else:
            self.logger.debug('%i is within the %i second timeout for a %s command' %
                              (dt, local_timeouts[command], command))
            
        return

    def throw_error(self):
        """
        Throw a general error that the DAQ is stuck
        """
        self.mongo.log_error(
                            "Dispatcher control loop can't get DAQ out of stuck state",
                            'ERROR',
                            "GENERAL_ERROR")

    def check_run_turnover(self, logical):
        """
        During normal operation we want to run for a certain number of minutes, then
        automatically stop and restart the run. No biggie. We check the time here
        to see if it's something we have to do.
        """
        det = list(latest_status[logical]['detectors'].keys())[0]
        number = self.latest_status[logical]['number']
        start_time = self.mongo.get_run_start(number)
        if start_time is None:
            self.logger.debug(f'No start time for {number}?')
            return
        time_now = now()
        run_length = int(self.goal_state[det]['stop_after'])*60
        run_duration = (time_now - start_time).total_seconds()
        self.logger.debug('Checking run turnover for %s: %i/%i' % (logical, run_duration, run_length))
        if run_duration > run_length:
            self.logger.info('Stopping run for %s' % logical)
            self.control_detector(logical, 'stop')

