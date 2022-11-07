import time
import socket
import subprocess
import datetime
import daqnt
import pytz
import typing as ty
from enum import IntEnum

__all__ = 'Hypervisor HypervisorAuthorization'.split()

def date_now():
    return datetime.datetime.now(pytz.utc)


class Hypervisor(object):
    __version__ = '4.0.4'
    def __init__(self,
                 db,
                 logger,
                 config,
                 vme_crates,
                 detector='tpc',
                 control_inputs=None,
                 slackbot=None,
                 testing=False):
        """
        Hypervisor, the daq resolver that restarts processes on request
            or if things are failing.
        :param db: DAQ database
        :param logger: logger
        :param config: the dispatcher config of the full system
        :param vme_crates: a dict of {number: address} VME crates to control
        :param detector: detector (either of 'tpc', 'muon_veto', or 'neutron_veto') this HV controls
        :param control_inputs: the list of control handles the dispatcher uses
        :param slackbot: optional slackbot messaging class
        :param testing: testing
        """
        if not isinstance(detector, str) or detector not in ['tpc', 'muon_veto', 'neutron_veto', 'test']:
            raise ValueError(f"Single detector only allowed: {detector} is unknown")
        self.db = db
        self.logger = logger
        self.detector = detector
        self.hosts = config[detector]['controller'] + config[detector]['readers']
        self.vme_crates = vme_crates
        self.physical_detectors = dict(config.items())
        self.control_inputs = [] if control_inputs is None else control_inputs[:]
        self.slackbot = slackbot
        self.sleep_time = {
            'long': 10,
            'short': 5,
            # Check on the status that the DAQ is up every 'poll' minutes
            'poll': 4 * 60,
            'max_wait': 15 * 60}
        # If we are not starting a run for this long, increase the
        # authorization on the Hypervisor
        self.max_timeout = 90 * 60
        self.testing = testing
        self.logger.info(f'HV v{self.__version__} started')

    def run_over_ssh(self, address: str, cmd: str, rets: list) -> None:
        """
        Runs a command via ssh
        :param address: username@host
        :param cmd: some command, wrapped in extra quotes
        :param rets: a list where return information is put
        :returns: None
        """
        command = "ssh " + address + ' ' + cmd
        try:
            cp = subprocess.run(command, shell=True, capture_output=True, timeout=30)
        except subprocess.TimeoutExpired:
            self.logger.error(f'Timeout while issuing command to {address}')
            rets.append({'retcode': -1, 'stdout': '', 'stderr': ''})
            return
        ret = {'retcode': cp.returncode,
               'stdout': cp.stdout.decode() if cp.stdout else '',
               'stderr': cp.stderr.decode() if cp.stderr else ''}
        rets.append(ret)

    def vme_control(self, crate: ty.Union[str, int], state: str) -> ty.Union[int, str]:
        """Exec state on crate x, return the return code 0"""
        cmd = f'$CMD:SET,CH:8,PAR:{state.upper()}\r\n'
        self.logger.info('Setting VME%s to %s' % (crate, state))
        if str(crate) not in self.vme_crates:
            self.logger.error(f'This HV doesn\'t have control over VME{crate}')
            self.logger.debug(f'Asks for {crate}, knows {self.vme_crates}')
            return 1
        try:
            with socket.create_connection((self.vme_crates[str(crate)], 8100), timeout=1) as s:
                s.sendall(cmd.encode())
                time.sleep(0.01)
                # clears the ethernet output buffer of the results of this
                # command, so it won't somehow accidentally wind up in the VME
                # monitor.
                _ = s.recv(1024)
                # s.shutdown(socket.SHUT_RDRW)
        except Exception as e:
            self.logger.error("Got %s: %s" % (type(e), str(e)))
            return str(e)
        return 0

    def fix_orphaned_sin(self, cc: str) -> int:
        """
        Fixes any potential orphaned S-IN by arming then disarming the CC.
        If you're particularly unlucky someone will have deleted the 'tpc_cause_crashes' mode in which
        case you're going to have to improvise a bit.
        """
        if not isinstance(cc, str) or 'controller' not in cc or cc not in self.hosts:
            self.logger.error(f'Cannot fix {cc} because you don\'t know what you\'re doing')
            return -1
        self.logger.info(f'Fixing orphaned S-IN from {cc}')
        doc = dict(command='arm', user='hypervisor', host=[cc], mode='tpc_cause_crashes',
            acknowledged={cc:0}, createdAt=date_now(), detector='tpc')
        oid = self.db.control.insert_one(doc)
        time.sleep(0.2)
        for i in range(5):
            if self.db.control.find_one({'_id': oid.inserted_id})['acknowledged'][cc] == 0:
                self.logger.info(f'S-IN not ack\'d yet ({i}/5)')
                time.sleep(self.sleep_time['short'])
            else:
                self.logger.info(f'S-IN is ack\'d ({i}/5)')
                break
        doc['command'] = 'stop'
        doc['createdAt'] = date_now()
        del doc['_id']
        self.db.control.insert_one(doc)
        time.sleep(0.2)
        self.logger.info(f'S-IN from {cc} fixed')
        return 0

    def start_redax(self, hosts: ty.Union[str, list], cycle_boards=True) -> ty.List[int]:
        """
        Starts redax on the specified host, maybe also resetting all its boards
        """
        if isinstance(hosts, str):
            hosts = [hosts]
        ret = []
        for h in hosts:
            if h not in self.hosts:
                self.logger.error(f'This HV doesn\'t have control over {h}')
                continue
            self.logger.info('Starting %s' % h)
            physical_host, process, id_ = h.split('_')
            cycle_boards = cycle_boards and process == 'reader'
            test = " --test" if self.testing else ""
            reset = ""
            if cycle_boards and not self.testing:
                self.logger.info(f'Cycling boards on {h}')
                reset = r"cd ~/read_reg && for l in {0..4}; do for b in {0..7}; do ./reset $l $b; done; done; "
            cmd = f'"source /daq_common/etc/daqrc ; {reset} cd /daq_common/daqnt/readers && ./start_process.sh --process {process} --id {id_}{test}"'
            ret_temp = []
            self.run_over_ssh(f'xedaq@{physical_host}', cmd, ret_temp)
            ret.append(ret_temp[0]['retcode'])
            if ret_temp[0]['stdout']:
                self.logger.debug(ret_temp[0]['stdout'])
            if ret_temp[0]['stderr']:
                self.logger.debug(ret_temp[0]['stderr'])
        return ret

    def stop_redax(self, hosts: ty.Union[str, list]) -> ty.List[int]:
        """Nicely ask redax to quit on host(s) return the return codes"""
        if isinstance(hosts, str):
            hosts = [hosts]
        for h in hosts:
            if h not in self.hosts:
                self.logger.error(f'This HV does\'t have control over {h}')
                hosts.remove(h)
        self.logger.info('Stopping %s' % hosts)
        self.db.control.insert_one({'command': 'quit', 'user': 'hypervisor',
                                    'host': hosts, 'acknowledged': {h: 0 for h in hosts},
                                    'detector': self.detector,
                                    'createdAt': date_now()})
        return [0] * len(hosts)

    def kill_redax(self, hosts: ty.Union[str, list]) -> ty.List[int]:
        """
        If the process is timing out it won't respond to a 'quit'
        :return: list of states of the readers
        """
        if isinstance(hosts, str):
            hosts = [hosts]
        ret = []
        for h in hosts:
            if h not in self.hosts:
                self.logger.error(f'This HV doesn\'t have control over {h}')
                continue
            self.logger.info('Killing %s' % h)
            physical_host, process, id_ = h.split('_')
            ret_temp = []
            self.run_over_ssh(f'xedaq@{physical_host}', f'"screen -X -S {process}{id_} quit"', ret_temp)
            ret.append(ret_temp[0]['retcode'])
            if ret_temp[0]['stdout']:
                self.logger.debug(ret_temp[0]['stdout'])
            if ret_temp[0]['stderr']:
                self.logger.debug(ret_temp[0]['stderr'])
        return ret

    def start_eventbuilders(self, hosts: ty.Union[str, list]):
        """
        hosts = [eb0,eb1,...,eb5] or either of those.
        Starts bootstrax, microstrax and ajax
        """
        raise NotImplementedError('Eventbuilders are not managed by hypervisor')
        # if isinstance(hosts, str):
        #     hosts = [hosts]
        # ret = []
        # for physical_host in hosts:
        #     cmd = f'ssh xedaq@{physical_host} "cd /daq_common/daqnt/event_builders && ./start_eventbuilder.sh"'
        #     cp = subprocess.run(cmd, shell=True, capture_output=True)
        #     ret.append(cp.returncode)
        #     if cp.stdout:
        #         self.logger.debug(cp.stdout.decode())
        #     if cp.stderr:
        #         self.logger.debug(cp.stderr.decode())
        # return ret

    def stop_eventbuilders(self, hosts: ty.Union[str, list]):
        """
        hosts = [eb0,eb1,...,eb5] or either of those.
        Closes bootstrax, microstrax and ajax
        """
        raise NotImplementedError("Eventbuilders are not controlled by the hypervisor")
        # if isinstance(hosts, str):
        #     hosts = [hosts]
        # ret = []
        # for physical_host in hosts:
        #     # I'm too lazy to properly scriptify this or learn proper bash
        #     cp = subprocess.run(
        #         f"ssh -t xedaq@{physical_host} "
        #         f"'screen -wipe ; "
        #         f"screen -X -S bootstrax quit;  "
        #         f"screen -X -S microstrax quit;  "
        #         f"screen -X -S ajax quit; "
        #         f"screen -wipe'", shell=True, capture_output=True)
        #     ret.append(cp.returncode)
        #     if cp.stdout:
        #         self.logger.debug(cp.stdout.decode())
        #     if cp.stderr:
        #         self.logger.debug(cp.stderr.decode())
        # return [daqnt.DAQ_STATUS.IDLE] * len(hosts)

    def redaxctl(self, action: str, target: ty.Union[str, list]) -> ty.List[int]:
        """Wrapper for redax actions. Return a list of redax states"""
        if action == 'start':
            return self.start_redax(target)
        elif action == 'stop':
            return self.stop_redax(target)
        elif action == 'kill':
            return self.kill_redax(target)
        raise ValueError(f'Unknown action {action} for redax control')

    def vmectl(self, action: str, target: ty.Union[int, str]) -> int:
        return self.vme_control(target, action)

    def bootstraxctl(self, action, target):
        """Not implemented"""
        pass

    def ajaxctl(self, action, target):
        """Not implemented"""
        pass

    def ebctl(self, action, target):
        """Not implemented"""
        pass

    def microstraxctl(self, action, target):
        """Not implemented"""
        pass

    def process_control(self,
                        extra_todo: ty.Union[list, tuple, None] = None) -> ty.Union[ty.List[list], ty.List[int]]:
        """
        Check whatever the request for the processController in the
            hypervisor collection and do it. Optionally, do the
            extra-todos AFTER the commands in the hypervisor collection.

        Commands from the DB should be ack'd to the DB, and commands
            from the dispatcher should be ack'd to the dispatcher.
        """
        coll = self.db.hypervisor
        doc = coll.find_one_and_update({'ack': 0},
                                       {'$currentDate': {'ack': 1}}, sort=[('_id', 1)])
        ret = []
        if doc is not None:
            self.logger.debug(f'Found {len(doc["commands"])} commands')
            for task in doc['commands']:
                self.logger.debug(f'{task}')
                command, action, target = task['command'], task['action'], task['target']
                try:
                    ret.append(getattr(self, command)(action, target))
                except Exception as e:
                    ret.append(str(e))
            coll.update_one({'_id': doc['_id']}, {'$set': {'ret': ret}})

        # Return return codes to dispatcher, not mongo and vice versa
        ret = []

        if extra_todo is not None:
            """Commands to be ackno"""
            for task in extra_todo:
                self.logger.debug(f'{task}')
                command, action, target = task['command'], task['action'], task['target']
                try:
                    return_code = getattr(self, command)(action, target)
                    ret.append(return_code)
                except Exception as e:
                    self.logger.error(f'Caught a {type(e)} while doing {command}: {action} '
                                      f'on {target}: {e}')
        return ret

    # def strategic_nuclear_option(self):
    #     """
    #     When even the tactical options aren't powerful enough
    #     """
    #     raise NotImplementedError("This feature isn't ready yet")
    #
    # def should_hypervisor_fix_things(self):
    #     """
    #     Should the hypervisor take action? Maybe return what level of action should be taken
    #     """
    #     pass

    def change_linking(self, logical_detectors):
        """
        Makes the input changes necessary to set the desired linking state
        :param logical_detectors: a list of groupings of physical detectors. Unlinked detectors must be strings,
            linked detectors must be a tuple. Eg: [('tpc', 'muon_veto'), 'neutron_veto']
        """
        config = self.db['global_settings'].find_one({'module': 'hypervisor'})
        self.logger.critical('Hypervisor unlinking detectors')

        # TODO we need to figure out how to handle non-background modes
        for detector in logical_detectors:
            if isinstance(detector, str):
                # unlinked detector
                detector = (detector,)
            self.logger.debug(f'Looking for new mode for {detector}')
            # this is a bit awkward. We can't use lists as keys in dicts/documents, only strings,
            # so the key is the name of the mode and the value is the list of detectors that the
            # mode applies to. It's backwards but that's just how it's gotta be
            for mode, dets in config['default_modes'].items():
                if sorted(detector) == sorted(dets):
                    self.logger.debug(f'Found a match for {mode}: {dets}')
                    for det in detector:
                        self.make_low_level_control_change(detector=det, field='mode', value=mode)
        return

    def make_low_level_control_change(self, detector: str, field: str, value: ty.Union[str,int]) -> int:
        """
        This function puts a new document into the control collection so the desired
        system status changes. We can't use the API because that will reject commands
        if the system is currently in a linked state, and that might be inconvenient
        :param detector: a physical detector, currently 'tpc', 'muon_veto', or 'neutron_veto'
        :param field: a valid control field, currently 'active', 'comment', 'mode', 'softstop', or 'stop_after'
            (note that 'remote' isn't valid at this level)
        :param value: the desired value for the specified field
        :returns: 0 if things worked, a negative number otherwise
        """
        if detector not in self.physical_detectors.keys():
            self.logger.error(f"'{detector}' isn't a valid detector, must be in {list(self.physical_detectors.keys())}")
            return -1
        if field not in self.control_inputs:
            self.logger.error(f"'{field}' isn't a valid control input, must be in {self.control_inputs}")
            return -1
        doc = dict(user='hypervisor', detector=detector, field=field, value=value,
                key=f'{detector}.{field}', time=date_now())
        self.logger.info(f'Setting {detector}.{field} to {value}')
        self.db.detector_control.insert_one(doc)
        return 0

    def handle_timeout(self, host: str):
        """
        This function gets called by the dispatcher if processes are timing out.
        :param host: the host to restart
        :return: None
        """
        self.logger.info('Restarting {host}')
        if self.slackbot is not None:
            self.slackbot.send_message(
                f'Hypervisor is restarting {host}',
                add_tags=('daq',))

        if 0 not in self.kill_redax(host):
            self.logger.error(f'Error killing {host}?')

        time.sleep(self.sleep_time['short'])

        # restart redax
        if 0 not in self.start_redax(host):
            self.logger.error(f'Error starting {host}?')
        time.sleep(self.sleep_time['long'])

    def linked_nuclear_option(self):
        """
        The system isn't responding as desired, and we're currently linked. If we can solve the problem by unlinking
        the offending detector, we do so here. If not, we proceed with the regular nuclear option. Returns a boolean
        of whether we think we fixed the problem here. We ask the owning dispatcher directly for the current system state
        so this won't work in testing.

        Also this assumes 3 detectors, that the HV doesn't get extended to support other detectors, and that Case E never
        gets used. Fix it if you don't like it.
        """
        if not hasattr(self, 'mongo_connect'):
            self.logger.error('Darryl hasn\'t made this work in testing yet')
            raise ValueError('This only works in prod')
        
        timeout_list, running_list = [], []
        physical_status = self.mongo_connect.physical_status
        latest_status = self.mongo_connect.latest_status
        host_config = self.mongo_connect.host_config
        phys_stat = {k: [] for k in config}
        now_time = time.time()
        for logical in latest_status.keys():
            log_status = latest_status[logical]["status"]
            for n, doc in latest_status[logical]['readers'].items():
                if doc is None:
                    self.logger.debug(f'{n} seems to have been offline for a few days')
                    continue
                det = host_config[doc['host']]
                status = self.mongo_connect.extract_status(doc, now_time)
                doc['status'] = status
                phys_stat[det].append(status)
                self.logger.debug(f'{det}: {doc["host"]} is in {status}')
            for n, doc in self.latest_status[logical]['controller'].items():
                if doc is None:
                    self.logger.debug(f'{n} seems to have been offline for a few days')
                    continue
                det = host_config[doc['host']]
                status = self.mongo_connect.extract_status(doc, now_time)
                doc['status'] = status
                phys_stat[det].append(status)
                self.logger.debug(f'{det}: {doc["host"]} is in {status}')
            for det in latest_status[logical]['detectors'].keys():
                phy_status = latest_status[logical]['detectors']['status']
                agg_status = self.mongo_connect.combine_statuses(phys_stat[det])
                self.logger.debug(f'{logical} is {log_status}, {det} is {phy_status} -> {agg_stat}')
                #if phy_status in [daqnt.DAQ_STATUS.TIMEOUT]: timeout_list.append(phys_det)
                #else: running_list.append(phys_det)
                if (phy_status != agg_status) or (phy_status in ['ERROR','TIMEOUT','UNKNOWN']):
                    timeout_list.append(det)
                else:
                    running_list.append(det)
                    
        self.logger.debug(f'These detectors are not in timeout: {running_list}, '
                          f'these are in timeout: {timeout_list}')
        # check if the TPC is part of the problem
        if 'tpc' in timeout_list:
            return False

        if len(timeout_list) == len(physical_status):
            self.logger.error('Uh, how did you get here???')
            self.slackbot.send_message('This happened again, you should really'
                    ' get someone to fix this', tags='ALL')
            raise ValueError('Why did this happen?')

        if self.slackbot is not None:
            self.slackbot.send_message('Hypervisor is unlinking detectors',
                                       add_tags='ALL')

        # ok, we aren't the problem, let's see about unlinking
        if len(running_list) == 1:
            # everyone else has died, it's just us left
            logical_detectors = physical_status.keys()
        elif self.mongo_connect.is_linked(running_list[0], running_list[1]):
            # the detector we are linked with is fine, the other one crashed
            logical_detectors = [running_list, timeout_list]
        else:
            # the detector we linked with crashed, and the other one is fine
            logical_detectors = physical_status.keys()
        self.logger.debug(f'Got these logical detectors: {logical_detectors}')
        self.change_linking(logical_detectors)
        return True

    def tactical_nuclear_option(self, is_linked=False) -> bool:
        """
        This function does whatever is necessary to get the readout to start, short of
        restarting servers
        :param is_linked: boolean, is the system currently linked?
        :returns: True if the weapons detonated, False if the bombers were called back first
        """
        self.logger.critical("Hypervisor invoking the tactical nuclear option")
        level = HypervisorAuthorization.HardReset
        if is_linked:
            # we're currently in a linked mode, lots of extra logic to handle first
            if self.linked_nuclear_option():
                # problem solved
                return False

        # guess not, let's drop some nukes
        if self.can_use_the_force():
            self.logger.warning('Hypervisor going Nuclear')
            level = HypervisorAuthorization.Nuclear
        self.hard_reset(level)
        return True

    def get_hypervisor_authorization(self):
        """Determine until which level the hypervisor is allowed to resolve conflicts"""
        self.logger.info(f'Determine authorization of hypervisor.')
        level = HypervisorAuthorization.Nothing
        since = date_now() - datetime.timedelta(seconds=self.sleep_time['poll'])
        is_running = self.was_daq_running(since)
        should_be_running = self.should_daq_be_running()

        self.logger.info(f'Since {since} the DAQ is running={is_running} '
                         f'which should be {should_be_running}')

        if is_running and should_be_running:
            # Nothing to do Hypervisor does not need to be authorized
            # since the DAQ is in the desired state
            self.logger.debug('Nothing to worry about, no authorization')
            return level
        if not is_running and not should_be_running:
            self.logger.debug('Nothing to worry about, we are off and should be off')
            return level
        if is_running and not should_be_running:
            self.logger.warning('DAQ is running when not supposed to? This '
                                'does not sound like a job for the hypervisor')
            return level

        assert (should_be_running and not is_running), "learn to elif please"

        level = HypervisorAuthorization.TimeoutResolve
        if self.daq_timedout_long():
            self.logger.warning('Hypervisor going to hard reset')
            level = HypervisorAuthorization.HardReset

            if self.can_use_the_force():
                self.logger.warning('Hypervisor going Nuclear')
                level = HypervisorAuthorization.Nuclear

        self.logger.info(f'Hypervisor authorized to level {str(level)}')
        return level

    def was_daq_running(self, since: datetime.datetime) -> bool:
        """Was the daq running since {since}"""
        for doc in self.db['aggregate_status'].find({'time': {'$gt': since}, 'detector': self.detector}):
            if doc['status'] == daqnt.DAQ_STATUS.RUNNING:
                self.logger.debug(f'Since {since}, the {self.detector.upper()} was in {doc["status"]}')
                return True
        self.logger.debug(f'Since {since}, the {self.detector.upper()} was not running')
        return False

    def should_daq_be_running(self) -> bool:
        """Query if the detector was set to active"""

        doc = self.db['detector_control'].find_one({'key': f'{self.detector}.active'},
                                                   sort=[('_id', -1)])
        res = doc is not None and doc['value'] == 'true'
        self.logger.debug(f'Should daq be running: {res}')
        return res

    def daq_timedout_long(self) -> bool:
        """
        Different way of checking that a run has started in the last
            self.max_timeout seconds
            """
        last_start = self.db['control'].find_one(
            {"acknowledged.reader0_controller_0": {"$ne": 0},
             "command": "start"},
            sort=[("_id", -1)])
        last_start_date = last_start['acknowledged']['reader0_controller_0'].replace(tzinfo=pytz.utc)
        time_since_last = date_now() - last_start_date
        self.logger.debug(f'Time since last run start {time_since_last}')
        return time_since_last.seconds > self.max_timeout

    def can_use_the_force(self):
        """
        Is the hypervisor allowed to use force to resolve issues

        Is so if we are either sleeping or have a weekend.
        """
        now = date_now()
        hour = now.hour
        weekday = now.weekday()
        working_hours = range(8, 21)
        self.logger.debug(f'can_use_the_force:: {hour not in working_hours}')
        return (hour not in working_hours) or (weekday in [5, 6])

    def get_current_readout_state(self, hosts: list) -> ty.Tuple[list, list, list]:
        """Get the states of the hosts"""
        responding, timeout, states = [], [], []
        now = date_now()
        for host in hosts:
            doc = self.db['status'].find_one({'host': host}, sort=[('_id', -1)])
            if doc is None or now - doc['time'].replace(tzinfo=pytz.utc) > datetime.timedelta(seconds=10):
                timeout.append(doc['host'])
            else:
                responding.append(doc['host'])
                states.append(doc['status'])
        return responding, timeout, states

    def hard_reset(self, authorization_level):
        """
        Hard resets by the hypervisor, either coming from the
            dispatcher or ensure_the_daq_is_up

        The function will try to reset however many systems as it is
            allowed to reset. This is done in accordance to the
            authorization level granted as an argument.Based on that,
            it can go through the various levels of rebooting:
             - HardReset/TimeoutResolve -> Forceful restart allowed
             - Nuclear -> everything shy of rebooting the servers at LNGS
        """
        if authorization_level not in [HypervisorAuthorization.TimeoutResolve,
                                       HypervisorAuthorization.HardReset,
                                       HypervisorAuthorization.Nuclear]:
            self.logger.warning("hard_reset is not available for Nothing")
            return

        all_readout = self.hosts[:]

        if authorization_level <= HypervisorAuthorization.HardReset:
            # Figure out what is running and what is in timeout
            responding, timeout, states = [], [], [daqnt.DAQ_STATUS.RUNNING]
            for i in range(int(self.sleep_time['max_wait'] // self.sleep_time['short'])):
                if not any(s in [daqnt.DAQ_STATUS.ARMING,
                                 daqnt.DAQ_STATUS.ARMED,
                                 daqnt.DAQ_STATUS.RUNNING]
                           for s in states):
                    break
                time.sleep(self.sleep_time['short'])
                responding, timeout, states = self.get_current_readout_state(all_readout)
                self.logger.info('Try %i Current states: %s/%s' % (i, states, timeout))
            else:
                self.logger.error(
                    f"Did not resolve host status in {self.sleep_time['max_wait']} s. "
                    f"Going for the nuclear option. There be dragons here")
                authorization_level = HypervisorAuthorization.Nuclear

        if authorization_level == HypervisorAuthorization.Nuclear:
            self.logger.critical("Doing a Nuclear reset, this might be nasty")
            responding, timeout, states = [], all_readout, []

        if hasattr(self, 'daq_controller'):
            self.daq_controller.control_detector(
                command='stop', detector='tpc', force=True)
        else:
            self.make_low_level_control_change(detector=self.detector, field='active', value='false')
            self.make_low_level_control_change(detector=self.detector, field='softstop', value='false')

        if self.slackbot is not None:
            self.slackbot.send_message(
                f'Hypervisor is fixing the DAQ. Status:\n'
                f'Responding {str(responding)}\n'
                f'Timeout {str(timeout)}\n'
                f'Level {str(authorization_level)}',
                add_tags='ALL')
        self.logger.info('%i responding, %i timeout' % (len(responding), len(timeout)))
        # all processes are either idle or timing out.
        # Make sure to first (force) quit redax instances prior to VMEs.
        if len(responding) > 0:
            self.stop_redax(responding)
        if len(timeout) > 0:
            self.kill_redax(timeout)
        time.sleep(self.sleep_time['long'])  # Make sure redax has shut down

        # Double check all readout is in timeout before turning off VMEs:
        _, timeout, _ = self.get_current_readout_state(all_readout)
        if not set(timeout) == set(all_readout):
            self.logger.fatal(f'Not all readout is in timeout, turning off VMEs might be bad')
            self.logger.info(f'Timeout: {timeout}\nReadout: {all_readout}')

        for c in self.vme_crates.keys():
            self.vme_control(c, 'off')

        time.sleep(self.sleep_time['long'])  # redax takes time to stop

        for c in self.vme_crates.keys():
            self.vme_control(c, 'on')
        time.sleep(self.sleep_time['long'])  # the boards take time to boot
        self.start_redax(all_readout)
        time.sleep(self.sleep_time['long'])  # give redax time to start
        for h in self.hosts:
            if 'controller' in h:
                self.fix_orphaned_sin(h)

        if hasattr(self, 'daq_controller'):
            # Might look a little funny but as far as the dispatcher is
            # concerned we should still be running, so no need to set to active
            pass
        else:
            self.make_low_level_control_change(detector=self.detector, field='active', value='true')

        self.logger.info('Hopefully that fixed it')

    def ensure_readout_is_up(self, preset_authorization_level=None):
        """
        The beating heart of the hypervisor as a stand-alone.

        The function will decide which parts it can reset, it
            first determines it's authorization. It can do either of:
             - HardReset -> Forceful restart allowed
             - Nuclear -> everything shy of rebooting LNGS

        This function infinitely loops with 'poll' timeout sleeps
            in between.
        """
        while not self.event.is_set():
            now = date_now()
            self.logger.info('Check at %s' % now.isoformat(sep=' '))
            if preset_authorization_level is None:
                self.logger.debug('Get authorization level')
                authorization_level = self.get_hypervisor_authorization()
            else:
                self.logger.debug(f'Authorization is preset to {preset_authorization_level}')
                authorization_level = preset_authorization_level

            self.logger.debug(f'Hypervisor is authorized as {str(authorization_level)}')
            if authorization_level < HypervisorAuthorization.HardReset:
                self.event.wait(self.sleep_time['poll'])
                continue

            # DAQ was not running and was supposed to be, let's inform the website that we are here
            self.logger.fatal("It's a bird, it's a plane, no it's the HYPERVISOR")
            t_start = time.time()
            self.hard_reset(authorization_level)
            t_end = time.time()
            self.event.wait(max(0, int(self.sleep_time['poll'] - (t_end - t_start))))


class HypervisorAuthorization(IntEnum):
    """Level of authorization granted to Hypervisor"""
    # In case we don't want the Hypervisor to do anything
    Nothing = 0
    # Tactical restart of timeout processes is allowed
    TimeoutResolve = 1
    # Forceful restart allowed
    HardReset = 2
    # Nuclear: everything shy of rebooting LNGS
    Nuclear = 3

