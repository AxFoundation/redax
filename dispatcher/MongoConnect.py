import datetime
from daqnt import DAQ_STATUS
import threading
import time
import pytz
import numpy as np


def encode_for_numpy(doc):
    if '_id' in doc:
        del doc['_id']
    mode_max_length = 32
    t = (
            doc['time'].isoformat().split('+')[0], # strip tz info
            doc['detector'],
            doc['number'],
            (doc['mode'] or '')[:mode_max_length].lower(),
            doc['rate'],
            doc['buff'],
            doc['status'],
            )
    dtype = [
            ('time', 'datetime64[us]'),
            ('detector', 'U16'),
            ('number', np.int32),
            ('mode', f'U{mode_max_length}'),
            ('rate', np.float32),
            ('buff', np.float32),
            ('status', np.int8)
            ]
    return t, dtype


def _all(values, target):
    return len(values) > 0 and all([v == target for v in values])

def now():
    return datetime.datetime.now(pytz.utc)

# Communicate between various parts of dispatcher that no new run was determined
NO_NEW_RUN = -1

class MongoConnect(object):
    """
    MongoDB Connectivity Class for XENONnT DAQ Dispatcher
    D. Coderre, 12. Mar. 2019
    D. Masson, 2019-2021
    S. di Pede, 2020-2021

    Brief: This code handles the mongo connectivity for both the DAQ 
    databases (the ones used for system-wide communication) and the 
    runs database. 

    """

    def __init__(self, config, daq_config, logger, control_mc, runs_mc, hypervisor, testing=False, logic_timer, gps_start_detectors, gps_period):

        # Define DB connectivity. Log is separate to make it easier to split off if needed
        dbn = config['ControlDatabaseName']
        rdbn = config['RunsDatabaseName']
        self.dax_db = control_mc[dbn]
        self.runs_db = runs_mc[rdbn]
        self.hypervisor = hypervisor

        self.latest_settings = {}

        self.loglevels = {"DEBUG": 0, "MESSAGE": 1, "WARNING": 2, "ERROR": 3, "FATAL": 4}

        # Each collection we actually interact with is stored here
        self.collections = {
            'incoming_commands': self.dax_db['detector_control'],
            'node_status': self.dax_db['status'],
            'aggregate_status': self.dax_db['aggregate_status'],
            'outgoing_commands': self.dax_db['control'],
            'log': self.dax_db['log'],
            'options': self.dax_db['options'],
            'run': self.runs_db[config['RunsDatabaseCollection']],
            'gps': self.runs_db['gps_sync'],
        }

        self.error_sent = {}

        # How often we should push certain types of errors (seconds)
        self.error_timeouts = {
            "ARM_TIMEOUT": 1, # 1=push all
            "START_TIMEOUT": 1,
            "STOP_TIMEOUT": 3600/4 # 15 minutes
        }
        # Timeout (in seconds). How long must a node not report to be considered timing out
        self.timeout = int(config['ClientTimeout'])

        # How long a node can be timing out or missed an ack before it gets fixed (TPC only)
        self.timeout_take_action = int(config['TimeoutActionThreshold'])

        # how long to give the CC to start the run. The +1 is so we check _after_ the CC should have acted
        self.cc_start_wait = int(config['StartCmdDelay']) + 1

        # Which control keys do we look for?
        self.control_keys = config['ControlKeys'].split()

        # a place to buffer commands temporarily
        self.command_queue = []
        self.q_mutex = threading.Lock()

        self.run_start_cache = {}

        # How often can we restart hosts?
        self.hypervisor_host_restart_timeout = int(config['HypervisorHostRestartTimeout'])
        self.host_is_timeout = set()

        self.digi_type = 'V17' if not testing else 'f17'
        self.cc_type = 'V2718' if not testing else 'f2718'

        # We will store the latest status from each reader here
        # Format:
        # {
        #    'tpc':   {
        #                'status': {enum},
        #                'mode': {string} run mode if any,
        #                'rate': {int} aggregate rate if any,
        #                'readers': {
        #                    'reader_0_reader_0': {
        #                           'status': {enum},
        #                           'rate': {float},
        #                     },
        #                 'controller': {}
        #                 }
        #  }
        self.latest_status = {}
        self.host_config = {}
        self.dc = daq_config
        self.hv_timeout_fix = {}
        for detector in self.dc:
            self.latest_status[detector] = {'readers': {}, 'controller': {}}
            for reader in self.dc[detector]['readers']:
                self.latest_status[detector]['readers'][reader] = {}
                self.host_config[reader] = detector
                self.hv_timeout_fix[reader] = now()
            for controller in self.dc[detector]['controller']:
                self.latest_status[detector]['controller'][controller] = {}
                self.host_config[controller] = detector
                self.hv_timeout_fix[controller] = now()

        self.should_backup_aggstat = True
        self.logger = logger
        self.run = True
        self.event = threading.Event()
        self.command_thread = threading.Thread(target=self.process_commands)
        self.command_thread.start()

        self.gps_start_detectors = gps_start_detectors
        self.gps_period = gps_period
        self.logic_timer = logic_timer

    def quit(self):
        self.run = False
        try:
            self.event.set()
            self.command_thread.join()
        except:
            pass

    def __del__(self):
        self.quit()

    def backup_aggstat(self):
        """
        Backs up the aggregate status collection by numpyizing it
        """
        today = now()
        coll = self.collections['aggregate_status']
        if today.day == 1 and self.should_backup_aggstat:
            then = (today - datetime.timedelta(days=31)).replace(tzinfo=None)
            self.logger.info(f'Backing up aggregated status older than {then.isoformat()}')
            data = []
            dtype = None
            for doc in coll.find({'time': {'$lt': then}}):
                try:
                    row, dtype = encode_for_numpy(doc)
                    data.append(row)
                except Exception as e:
                    self.logger.debug(f'Error encoding {doc}')
                    return
            try:
                data = np.array(data, dtype=dtype)
                # TODO better place to store them?
                with open(f'/daq_common2/logs/aggstat_{then.year}{then.month:02d}{then.day:02d}.npz', 'wb') as f:
                    np.savez_compressed(f, data=data)
            except Exception as e:
                self.logger.error(f'Caught a {type(e)} while numpyizing aggstat: {e}')
            else:
                ret = coll.delete_many({'time': {'$lt': then}})
                self.logger.debug(f'Backed up {ret.deleted_count} docs')
                self.should_backup_aggstat = False
        else:
            self.should_backup_aggstat = today.day != 1


    def get_update(self, dc):
        """
        Gets the latest documents from the database for
        each node we know about
        """
        try:
            sort=[('_id', -1)]
            for detector in dc.keys():
                for host in dc[detector]['readers'].keys():
                    doc = self.collections['node_status'].find_one({'host': host},
                                                                   sort=sort)
                    dc[detector]['readers'][host] = doc
                for host in dc[detector]['controller'].keys():
                    doc = self.collections['node_status'].find_one({'host': host},
                                                                    sort=sort)
                    dc[detector]['controller'][host] = doc
        except Exception as e:
            self.logger.error(f'Got error while getting update: {type(e)}: {e}')
            return None

        self.latest_status = dc

        # Now compute aggregate status
        return self.latest_status if self.aggregate_status() is None else None

    def time_to_next_gps(self):
        """
        In how many seconds can we expect the next GPS signal?
        """
        # how long ago was the most recent signal?
        utc = self.get_gps_time()[2]
        dt = now().timestamp() - utc
        return self.gps_period - dt

    def get_gps_time(self):
        """
        Return the most recent GPS timestamp, and its UTC equivalent (approx)
        """
        doc = self.collections['gps'].find_one({'channel': 0}, sort=[('_id', -1)])
        return doc['gps_sec'], doc['gps_ns'], int(str(doc['_id'])[:8], 16)

    def clear_error_timeouts(self):
        self.error_sent = {}

    def aggregate_status(self):
        """
        Compute the total status of each "detector" based on the most recent
        updates of its individual nodes. Here are some general rules:
         - Usually all nodes have the same status (i.e. 'running') and this is
           not very complicated
         - During changes of state (i.e. starting a run) some nodes might
           be faster than others. In this case the status can be 'unknown'.
           The main program should interpret whether 'unknown' is a reasonable
           thing, like was a command sent recently? If so then sure, a 'unknown'
           status will happpen.
         - If any single node reports error then the whole thing is in error
         - If any single node times out then the whole thing is in timeout
         - Rates, buffer usage, and PLL counters only apply to the physical
           detector, not the logical detector, while status and run number
           apply to both
        """
        now_time = time.time()
        ret = None
        aggstat = {
                k:{ 'status': -1,
                    'detector': k,
                    'rate': 0,
                    'time': now(),
                    'buff': 0,
                    'mode': None,
                    'pll_unlocks': 0,
                    'number': -1}
                for k in self.dc}
        phys_stat = {k: [] for k in self.dc}
        for detector in self.latest_status.keys():
            # detector = logical
            statuses = {}
            status = None
            modes = []
            run_nums = []
            for n, doc in self.latest_status[detector]['readers'].items():
                if doc is None:
                    self.logger.debug(f'{n} seems to have been offline for a few days')
                    continue
                phys_det = self.host_config[doc['host']]
                try:
                    aggstat[phys_det]['rate'] += doc['rate']
                    aggstat[phys_det]['buff'] += doc['buffer_size']
                    aggstat[phys_det]['pll_unlocks'] += doc.get('pll', 0)
                except Exception as e:
                    # This is not really important but it's nice if we have it
                    self.logger.debug(f'Rate calculation ran into {type(e)}: {e}')
                    pass

                status = self.extract_status(doc, now_time)
                statuses[doc['host']] = status
                phys_stat[phys_det].append(status)

            for n, doc in self.latest_status[detector]['controller'].items():
                if doc is None:
                    self.logger.debug(f'{n} seems to have been offline for a few days')
                    modes.append('none')
                    run_nums.append(-1)
                    statuses['none'] = DAQ_STATUS.UNKNOWN
                    continue
                phys_det = self.host_config[doc['host']]
                status = self.extract_status(doc, now_time)
                statuses[doc['host']] = status
                doc['status'] = status
                modes.append(doc.get('mode', 'none'))
                run_nums.append(doc.get('number', None))
                aggstat[phys_det]['status'] = status
                aggstat[phys_det]['mode'] = modes[-1]
                aggstat[phys_det]['number'] = run_nums[-1]
                phys_stat[phys_det].append(status)

            mode = modes[0]
            run_num = run_nums[0]
            if not _all(modes, mode) or not _all(run_nums, run_num):
                self.logger.error(f'No quorum? {modes}, {run_nums}')
                status_list = [DAQ_STATUS.UNKNOWN]
                mode = 'none'
                run_num = -1
            elif mode != 'none': # readout is "active":
                a,b = self.get_hosts_for_mode(mode)
                active = a + b
                status_list = [v for k,v in statuses.items() if k in active]
            else:
                status_list = list(statuses.values())

            # Now we aggregate the statuses
            status = self.combine_statuses(status_list)

            self.latest_status[detector]['status'] = status
            self.latest_status[detector]['number'] = run_num
            self.latest_status[detector]['mode'] = mode

        try:
            self.collections['aggregate_status'].insert_many(aggstat.values())
        except Exception as e:
            self.logger.error(f'DB snafu? Couldn\'t update aggregate status. '
                            f'{type(e)}, {e}')

        self.physical_status = phys_stat
        return ret

    def combine_statuses(self, status_list):
        # First, the "or" statuses
        for stat in ['ARMING','ERROR','TIMEOUT','UNKNOWN']:
            if DAQ_STATUS[stat] in status_list:
                return DAQ_STATUS[stat]
        # then the "and" statuses
        for stat in ['IDLE','ARMED','RUNNING']:
            if _all(status_list, DAQ_STATUS[stat]):
                return DAQ_STATUS[stat]
        return DAQ_STATUS.UNKNOWN

    def extract_status(self, doc, now_time):
        try:
            return DAQ_STATUS.TIMEOUT if self.is_timeout(doc, now_time) else DAQ_STATUS(doc['status'])
        except Exception as e:
            self.logger.debug(f'Setting status to unknown for {doc.get("host", "unknown")} because of {type(e)}: {e}')
            return DAQ_STATUS.UNKNOWN

    def is_timeout(self, doc, t):
        """
        Checks to see if the specified status doc corresponds to a timeout situation
        """
        host = doc['host']
        dt = t - int(str(doc['_id'])[:8], 16)
        has_ackd = self.host_ackd_command(host)
        ret = False
        self.logger.debug(f'{host} last reported {int(dt)} sec ago')
        if dt > self.timeout:
            ret = ret or True
        if has_ackd is not None and t - has_ackd > self.timeout_take_action:
            if host not in self.host_is_timeout:
                self.logger.critical(f'{host} hasn\'t ackd a command from {int(t-has_ackd)} sec ago')
                self.host_is_timeout.add(host)
            if self.host_config[host] == 'tpc':
                dt = (now() - self.hv_timeout_fix[host]).total_seconds()
                if dt > self.hypervisor_host_restart_timeout:
                    self.log_error(f'Hypervisor fixes timeout of {host}', "ERROR", "ERROR")
                    self.hypervisor.handle_timeout(host)
                    self.hv_timeout_fix[host] = now()
                else:
                    self.logger.debug(f'Not restarting {host}, timeout at {int(dt)}')
            ret = ret or True
        if not ret and host in self.host_is_timeout:
            self.host_is_timeout.discard(host)
        return ret

    def get_wanted_state(self):
        """
        Figure out what the system is supposed to be doing right now
        """
        try:
            latest_settings = {}
            for detector in self.dc:
                latest = None
                latest_settings[detector] = {}
                for key in self.control_keys:
                    doc = self.collections['incoming_commands'].find_one(
                            {'key': f'{detector}.{key}'}, sort=[('_id', -1)])
                    if doc is None:
                        self.logger.error(f'No key {key} for {detector}???')
                        return None
                    latest_settings[detector][doc['field']] = doc['value']
                    if latest is None or doc['time'] > latest:
                        latest = doc['time']
                        latest_settings[detector]['user'] = doc['user']
            self.goal_state = latest_settings
            return self.goal_state
        except Exception as e:
            self.logger.debug(f'get_wanted_state failed due to {type(e)} {e}')
            return None

    def is_linked_mode(self):
        """
        Are we in a linked configuration for this control iteration?
        """
        # self.dc has the physical detectors, self.latest_status has the logical detectors
        return len(self.dc.keys()) != len(self.latest_status.keys())

    def is_linked(self, a, b):
        """
        Check if the detectors are in a compatible linked configuration.
        """
        mode_a = self.goal_state[a]["mode"]
        mode_b = self.goal_state[b]["mode"]
        if mode_a != mode_b:
            self.logger.debug(f'{a} and {b} are not linked ({mode_a}/{mode_b})')
            # shortcut to lessen the load on the db
            return False

        # we don't need to pull the whole combined document because the 'detector' field is at the top level
        if (doc := self.collections['options'].find_one({'name': mode_a}, {'detector': 1})) is not None:
            detectors = doc['detector']
        else:
            detectors = None

        # Check if the linked detectors share the same run mode and
        # if they are both present in the detectors list of that mode
        # also no "tpc_muon_veto" bullshit, it must be ['tpc', 'muon_veto']
        if isinstance(detectors, list) and a in detectors and b in detectors:
            self.logger.debug(f'{a} and {b} are linked ({mode_a}/{detectors})')
            return True
        else:
            self.logger.debug(f'{a} and {b} aren\'t link?? How this happen?? {mode_a} {detectors}')
            return False

    def get_super_detector(self):
        """
        Get the Super Detector configuration
        if the detectors are in a compatible linked mode.
        - case A: tpc, mv and nv all linked
        - case B: tpc, mv and nv all un-linked
        - case C: tpc and mv linked, nv un-linked
        - case D: tpc and nv linked, mv un-linked
        - case E: tpc unlinked, mv and nv linked
        We will check the compatibility of the linked mode for a pair of detectors per time.
        """
        ret = {'tpc': {'controller': self.dc['tpc']['controller'][:],
                       'readers': self.dc['tpc']['readers'][:],
                       'detectors': ['tpc']}}
        mv = self.dc['muon_veto']
        nv = self.dc['neutron_veto']

        tpc_mv = self.is_linked('tpc', 'muon_veto')
        tpc_nv = self.is_linked('tpc', 'neutron_veto')
        mv_nv = self.is_linked('muon_veto', 'neutron_veto')

        # tpc and muon_veto linked mode
        if tpc_mv:
            # case A or C
            ret['tpc']['controller'] += mv['controller']
            ret['tpc']['readers'] += mv['readers']
            ret['tpc']['detectors'] += ['muon_veto']
        else:
            # case B or E
            ret['muon_veto'] = {'controller': mv['controller'][:],
                                'readers': mv['readers'][:],
                                'detectors': ['muon_veto']}
        if tpc_nv:
            # case A or D
            ret['tpc']['controller'] += nv['controller'][:]
            ret['tpc']['readers'] += nv['readers'][:]
            ret['tpc']['detectors'] += ['neutron_veto']
        elif mv_nv and not tpc_mv:
            # case E
            ret['muon_veto']['controller'] += nv['controller'][:]
            ret['muon_veto']['readers'] += nv['readers'][:]
            ret['muon_veto']['detectors'] += ['neutron_veto']
        else:
            # case B or C
            ret['neutron_veto'] = {'controller': nv['controller'][:],
                                   'readers': nv['readers'][:],
                                   'detectors': ['neutron_veto']}

        # convert the host lists to dics for later
        for det in list(ret.keys()):
            ret[det]['controller'] = {c:{} for c in ret[det]['controller']}
            ret[det]['readers'] = {c:{} for c in ret[det]['readers']}
        return ret

    def get_run_mode(self, mode):
        """
        Pull a run doc from the options collection and add all the includes
        """
        if mode is None:
            return None
        base_doc = self.collections['options'].find_one({'name': mode})
        if base_doc is None:
            self.log_error(f"Mode '{mode}' doesn't exist", "info", "info")
            return None
        if 'includes' not in base_doc or len(base_doc['includes']) == 0:
            return base_doc
        try:
            if self.collections['options'].count_documents({'name':
                {'$in': base_doc['includes']}}) != len(base_doc['includes']):
                self.log_error(f"At least one subconfig for mode '{mode}' doesn't exist", "WARNING", "WARNING")
                return None
            return list(self.collections["options"].aggregate([
                {'$match': {'name': mode}},
                {'$lookup': {'from': 'options', 'localField': 'includes',
                    'foreignField': 'name', 'as': 'subconfig'}},
                {'$addFields': {'subconfig': {'$concatArrays': ['$subconfig', ['$$ROOT']]}}},
                {'$unwind': '$subconfig'},
                {'$group': {'_id': None, 'config': {'$mergeObjects': '$subconfig'}}},
                {'$replaceWith': '$config'},
                {'$project': {'_id': 0, 'description': 0, 'includes': 0, 'subconfig': 0}},
                ]))[0]
        except Exception as e:
            self.logger.error(f"Got a {type(e)} exception in doc pulling: {e}")
        return None

    def get_hosts_for_mode(self, mode, detector=None):
        """
        Get the nodes we need from the run mode
        """
        if mode is None or mode == 'none':
            if detector is None:
                self.logger.error('No mode, no detector? wtf?')
                return [], []
            return (list(self.latest_status[detector]['readers'].keys()),
                    list(self.latest_status[detector]['controller'].keys()))
        if (doc := self.get_run_mode(mode)) is None:
            self.logger.error('How did this happen?')
            return [], []
        cc = []
        hostlist = []
        for b in doc['boards']:
            if self.digi_type in b['type'] and b['host'] not in hostlist:
                hostlist.append(b['host'])
            elif b['type'] == self.cc_type and b['host'] not in cc:
                cc.append(b['host'])
        return hostlist, cc

    def get_next_run_number(self):
        try:
            cursor = self.collections["run"].find({},{'number': 1}).sort("number", -1).limit(1)
        except Exception as e:
            self.logger.error(f'Database is having a moment? {type(e)}, {e}')
            return NO_NEW_RUN
        if cursor.count() == 0:
            self.logger.info("wtf, first run?")
            return 0
        return list(cursor)[0]['number']+1

    def set_stop_time(self, number, detectors, force):
        """
        Sets the 'end' field of the run doc to the time when the STOP command was ack'd
        """
        self.logger.info(f"Updating run {number} with end time ({detectors})")
        if number == -1:
            return
        try:
            time.sleep(0.5) # this number depends on the CC command polling time
            if (endtime := self.get_ack_time(detectors, 'stop') ) is None:
                self.logger.debug(f'No end time found for run {number}')
                endtime = now() -datetime.timedelta(seconds=1)
            query = {"number": int(number), "end": None, 'detectors': detectors}
            updates = {"$set": {"end": endtime}}
            if force:
                updates["$push"] = {"tags": {"name": "_messy", "user": "daq",
                    "date": now()}}
            if self.collections['run'].update_one(query, updates).modified_count == 1:
                self.logger.debug('Update successful')
                rate = {}
                for doc in self.collections['aggregate_status'].aggregate([
                    {'$match': {'number': number}},
                    {'$group': {'_id': '$detector',
                                'avg': {'$avg': '$rate'},
                                'max': {'$max': '$rate'}}}
                    ]):
                    rate[doc['_id']] = {'avg': doc['avg'], 'max': doc['max']}
                channels = set()
                if 'tpc' in detectors:
                    # figure out which channels weren't running
                    readers = list(self.latest_status[detectors]['readers'].keys())
                    for doc in self.collections['node_status'].find({'host': {'$in': readers}, 'number': int(number)}):
                        channels |= set(map(int, doc['channels'].keys()))
                updates = {'rate': rate}
                if len(channels):
                    updates['no_data_from'] = sorted(list(set(range(494)) - channels))
                self.collections['run'].update_one({'number': int(number)},
                                                   {'$set': updates})
                if str(number) in self.run_start_cache:
                    del self.run_start_cache[str(number)]
            else:
                self.logger.debug('No run updated?')
        except Exception as e:
            self.logger.error(f"Database having a moment, hope this doesn't crash. {type(e)}, {e}")
        return

    def get_ack_time(self, detector, command, recurse=True):
        '''
        Finds the time when specified detector's crate controller ack'd the specified command
        '''
        # the first cc is the "master", so its ack time is what counts
        cc = list(self.latest_status[detector]['controller'].keys())[0]
        query = {'host': cc, f'acknowledged.{cc}': {'$ne': 0}, 'command': command}
        sort = [('_id', -1)]
        doc = self.collections['outgoing_commands'].find_one(query, sort=sort)
        dt = (now() - doc['acknowledged'][cc].replace(tzinfo=pytz.utc)).total_seconds()
        if dt > 30: # TODO make this a config value
            if recurse:
                # No way we found the correct command here, maybe we're too soon
                self.logger.debug(f'Most recent ack for {detector}-{command} is {dt:.1f}?')
                time.sleep(2) # if in doubt
                return self.get_ack_time(detector, command, False)
            else:
                # Welp
                self.logger.debug(f'No recent ack time for {detector}-{command}')
                return None
        return doc['acknowledged'][cc]

    def send_command(self, command, hosts, user, detector, mode="", delay=0, force=False):
        """
        Send this command to these hosts. If delay is set then wait that amount of time
        """
        number = None
        ls = self.latest_status[detector]
        for host_list in hosts:
            for h in host_list:
                if h not in ls['readers'] and h not in ls['controller']:
                    self.logger.error(f'Trying to issue a {command} to {detector}/{h}?')
                    host_list.remove(h)
        if command == 'stop' and not self.detector_ackd_command(detector, 'stop'):
            self.logger.error(f"{detector} hasn't ack'd its last stop, let's not flog a dead horse")
            if not force:
                return 1
        try:
            if command == 'arm':
                number = self.get_next_run_number()
                if number == NO_NEW_RUN:
                    return -1
                self.latest_status[detector]['number'] = number
            doc_base = {
                "command": command,
                "user": user,
                "detector": detector,
                "mode": mode,
                "createdAt": now()
            }
            if command == 'arm':
                doc_base['options_override'] = {'number': number}
            if delay == 0:
                docs = doc_base
                docs['host'] = hosts[0]+hosts[1]
                docs['acknowledged'] = {h:0 for h in docs['host']}
                docs = [docs]
            else:
                docs = [dict(doc_base.items()), dict(doc_base.items())]
                docs[0]['host'], docs[1]['host'] = hosts
                docs[0]['acknowledged'] = {h:0 for h in docs[0]['host']}
                docs[1]['acknowledged'] = {h:0 for h in docs[1]['host']}
                docs[1]['createdAt'] += datetime.timedelta(seconds=delay)
            with self.q_mutex:
                self.command_queue += docs
        except Exception as e:
            self.logger.debug(f'SendCommand ran into {type(e)}, {e})')
            return -1
        else:
            self.logger.debug(f'Queued {command} for {detector}')
            self.event.set()
        return 0

    def process_commands(self):
        """
        Process our internal command queue
        """
        outgoing = self.collections['outgoing_commands']
        while self.run == True:
            try:
                with self.q_mutex:
                    if len(self.command_queue) > 1:
                        self.command_queue.sort(key=lambda d : d['createdAt'].timestamp())
                    if len(self.command_queue) > 0:
                        next_cmd = self.command_queue[0]
                        dt = (next_cmd['createdAt'].replace(tzinfo=pytz.utc) - now()).total_seconds()
                    else:
                        dt = 10
                if dt < 0.01:
                    with self.q_mutex:
                        outgoing.insert_one(self.command_queue.pop(0))
            except Exception as e:
                dt = 10
                self.logger.error(f"DB down? {type(e)}, {e}")
            self.event.clear()
            self.event.wait(dt)

    def host_ackd_command(self, host):
        """
        Finds the timestamp of the oldest unacknowledged command send to the specified host
        :param host: str, the process name to check
        :returns: float, the timestamp of the last unack'd command, or None if none exist
        """
        q = {'host': host, f'acknowledged.{host}': 0}
        sort = [('_id', 1)]
        if (doc := self.collections['outgoing_commands'].find_one(q, sort=sort)) is None:
            self.logger.debug(f'No unack\'d commands for {host}')
            return None
        return doc['createdAt'].replace(tzinfo=pytz.utc).timestamp()

    def detector_ackd_command(self, detector, command):
        """
        Finds when the specified/most recent command was ack'd
        """
        q = {'detector': detector}
        sort = [('_id', -1)]
        if command is not None:
            q['command'] = command
        if (doc := self.collections['outgoing_commands'].find_one(q, sort=sort)) is None:
            self.logger.error('No previous command found?')
            return True
        # we can't naively use everything in the hosts field, because we might be transitioning
        # out of linked mode, and there might be "garbage" in the host list because someone
        # didn't follow very clear instructions, and if a stop is issued to a host that doesn't
        # exist, the dispatcher basically stops working
        hosts_this_detector = set(self.latest_status[detector]['readers'].keys()) | set(self.latest_status[detector]['controller'].keys())
        hosts_in_doc = set(doc['host'])
        hosts_ignored = hosts_in_doc - hosts_this_detector
        if len(hosts_ignored):
            self.logger.warning(f'Ignoring hosts: {hosts_ignored}')
        # so we only loop over the intersection of this detector's hosts and the doc's hosts
        for h in hosts_this_detector & hosts_in_doc:
            if doc['acknowledged'][h] == 0:
                return False
        return True

    def log_error(self, message, priority, etype):
        #Start by logging the error localy
        self.logger.info(message)
        # Note that etype allows you to define timeouts.
        nowtime = now()
        if ( (etype in self.error_sent and self.error_sent[etype] is not None) and
             (etype in self.error_timeouts and self.error_timeouts[etype] is not None) and 
             (nowtime-self.error_sent[etype]).total_seconds() <= self.error_timeouts[etype]):
            self.logger.debug(f"Could log error, but still in timeout for type {type}")
            return
        self.error_sent[etype] = nowtime
        try:
            self.collections['log'].insert({
                "user": "dispatcher",
                "message": message,
                "priority": self.loglevels[priority]
            })
        except Exception as e:
            self.logger.error(f'Database error, can\'t issue error message: {type(e)}, {e}')
        self.logger.info(f"Error message from dispatcher: {message}")
        return

    def get_run_start(self, number):
        """
        Returns the timezone-corrected run start time from the rundoc
        """
        if str(number) in self.run_start_cache:
            return self.run_start_cache[str(number)]
        try:
            doc = self.collections['run'].find_one({"number": number}, {"start": 1})
        except Exception as e:
            self.logger.error(f'Database is having a moment: {type(e)}, {e}')
            return None
        if doc is not None and 'start' in doc:
            self.run_start_cache[str(number)] = doc['start'].replace(tzinfo=pytz.utc)
            return self.run_start_cache[str(number)]
        return None

    def insert_run_doc(self, detector):

        if (number := self.get_next_run_number()) == NO_NEW_RUN:
            self.logger.error("DB having a moment")
            return -1
        # the rundoc gets the physical detectors, not the logical
        detectors = self.latest_status[detector]['detectors']

        run_doc = {
            "number": number,
            'detectors': detectors,
            'user': self.goal_state[detector]['user'],
            'mode': self.goal_state[detector]['mode'],
            'bootstrax': {'state': None},
            'end': None,
            'gps_start': [0,0],
        }

        # If there's a source add the source. Also add the complete config
        cfg = self.get_run_mode(self.goal_state[detector]['mode'])
        if cfg is not None and 'source' in cfg.keys():
            run_doc['source'] = str(cfg['source'])
        run_doc['daq_config'] = cfg

        # If the user started the run with a comment add that too
        if "comment" in self.goal_state[detector] and self.goal_state[detector]['comment'] != "":
            run_doc['comments'] = [{
                "user": self.goal_state[detector]['user'],
                "date": now(),
                "comment": self.goal_state[detector]['comment']
            }]

        # Make a data entry so bootstrax can find the thing
        if 'strax_output_path' in cfg:
            run_doc['data'] = [{
                'type': 'live',
                'host': 'daq',
                'location': cfg['strax_output_path']
            }]

        if detector in self.gps_start_detectors:
            # wait for GPS signal to come along
            time.sleep(self.time_to_next_gps()+0.5)
            try:
                gps_sec, gps_ns, utc = self.get_gps_time()
                if (now().timestamp() - utc) < self.logic_timer:
                    # we got the freshest entry
                    start_time = datetime.datetime.fromtimestamp(gps_sec + gps_ns/1e9,
                            tz=pytz.utc)
                    run_doc['gps_start'] = [gps_sec, gps_ns]
                else:
                    self.logger.error(f'GPS timestamp too old? {gps_sec}, {gps_ns}, {utc}')
                    start_time = None
            except Exception as e:
                self.logger.error('Couldn\'t find a start time')
                start_time = None
        else:
            # The cc needs some time to get started
            time.sleep(self.cc_start_wait)
            try:
                start_time = self.get_ack_time(detector, 'start')
            except Exception as e:
                self.logger.error('Couldn\'t find start time ack')
                start_time = None

        if start_time is None:
            start_time = now()-datetime.timedelta(seconds=2)
            # if we miss the ack time, we don't really know when the run started
            # so may as well tag it
            run_doc['tags'] = [{'name': 'messy', 'user': 'daq', 'date': start_time}]
        run_doc['start'] = start_time

        try:
            self.collections['run'].insert_one(run_doc)
        except Exception as e:
            self.logger.error(f'Database having a moment: {type(e)}, {e}')
            return -1
        return None
