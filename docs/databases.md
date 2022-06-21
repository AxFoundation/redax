## Contents
* [Intro](index.md) 
* [Pre-install](prerequisites.md) 
* [DB config](databases.md) 
* [Installation](installation.md) 
* [Options reference](daq_options.md) 
* [Dispatchers](dispatcher.md)
* [Example operation](how_to_run.md)
* [Extending redax](new_digi.md)
* [Waveform simulator](fax.md)

# Configuration of Backend Databases

There are two separate databases defined in the DAQ: the DAQ database and the runs database.
In principle they could both point to the same deployment, or they could not.
This is meant to give flexibility since the DAQ-internal database should *definitely* be installed somewhere local to the system (since outages bring the whole thing down) while the run database should just *probably* be installed local to the system (since temporary outages may go unnoticed).
Even if both databases are locally installed, it may make sense to put them in separate deployments because of different replication rules.
The runs database should probably be replicated out to remote computing sites for easy access while the DAQ database is fine with a more localized replication strategy, since in the latter case the replication is more to serve backup than availability.

This section will list each collection in each database, explain what it is for, and give the document schema.
Note that since this is noSQL you can consider this a 'minimum document schema', as in the code expects these fields to be there but additional fields can be present as well.
This is especially pertinent to run documents.

## The DAQ Database

### db.status

The status collection should be configured as either a capped collection or with a TTL index on the "time" field (see the 'helpers' directory for a script to set up the necessary capped collections).
Each readout client reports its status to this DB every second.
The dispatcher queries the newest document from each node (considering also the time stamp of the document) to determine the aggregate DAQ state.

The form of the document is the following:
```python
{
    "host":   "xedaq00_reader_0", # DAQ name of client
    "time": <date object>, # the time when the document was made
    "status": 0,         # status enum
    "rate":  13.37,         # data rate in MB since last update
    "buffer_size" : 4.3,  # current buffer utilization in MB
    "run_mode" : "background_stable", # current run mode
    "channels" : {0 : 67,       # Rate per channel on this host in kB since last update
                  19 : 16,
                  ...
    },
}
```
Note that documents from a Crate Controller instance will also have a "run_number" field. The status enum has the following values:

|Value	|State |
| ----- | ----- |
|0	|Idle |
|1	|Arming (in the process of initializing, baselines, etc) |
|2	|Armed. Ready to start |
|3	|Running |
|4	|Error |
|5	|Timeout |
|6	|Unknown |

These values are valid throughout the entire DAQ.

### db.aggregate_status

This collection should also be configured as **capped**.
It is written to by the dispatcher only and provides the status history of the system on a detector level, which is a more palatable thing for operators to deal with.
It is mostly redundant compared to status, except that it depends also on the state document given to the dispatcher.
The provided dispatcher will automatically clean out documents from this collection at the start of every month, numpy-fying and compressing them.

The document format is similar to status:
```python
{ 
    "status" : 0,                                    // status enum, see status collection
    "number" : null,                                 // current run number (here Idle so null)
    "detector" : "tpc",                              // detector string
    "rate" : 0,                                      // data rate MB/s float
    "readers" : 2,                                   // number of readers connected
    "time" : ISODate("2018-09-20T13:35:05.642Z"),    // time document inserted
    "buff" : 0,                                      // total buffered data from all readers float
    "mode" : null                                    // run mode string (null because Idle here)
}
```

### db.detector_control

The documents in this collection are used to set the goal state of each detector.
There will be multiple documents here, and the dispatcher aggregates them together to figure out what should be done.
Upon update the dispatcher will attempt to recitfy the current actual state of the DAQ with the goal state provided in this document.

Because some of these fields require slightly more explanation a table has been included below in lieu of inline comments.

```python
{ 
        "detector" : "tpc", 
        "field": "active",
        "value": "true",
        "user" : "Coderre",
        "time": <Date instance>
}  
```

|Field	|Description |
| ----- | ----- |
|detector	|Either 'tpc', 'muon_veto', or 'neutron_veto'. Or whatever funny thing you've got in your lab. |
|field  | Which specific field this document is setting. Required fields are: "active", "stop_after", "finish_run_on_stop", "mode", "comment", "link_nv", "link_mv", "remote". See below for details. |
|value  | The value for the specified field. Things like "false", "5", "false", "vent_xenon_mode", etc. See below for details. |
|user	|Who gets credit/blame for starting these runs? This is the user who last changed this command doc and it will be recorded in the run documents of all runs recorded while this command is active. |
|time   |When this document was inserted. The user field of the most recent document is the current DAQ controller. |
|field: active	|The user can set whether this detector is 'active' or not. If it's not active then we don't care about its status. In fact we can't care since some readers will be reused when running in combined modes and may not longer belong to their original detectors.|
|field: stop_after	|How many minutes until the run automatically restarts. This is a global DAQ state setting, not the setting for a single run. So if you want to run for an hour you set this to 60 minutes, put the detector active, and the dispatcher should handle giving you the 1 hour runs. |
|field: finish_run_on_stop |How to deal with a run in progress if you set active to 'false'. If 'finish_run_on_stop' is true, we wait for the run to finish due to stop_after (but no new one is started). If false, we stop the run. Has no effect if active is 'true'. |
|field: mode	|The options mode we're running the DAQ in. Should correspond to the 'name' field of one of the documents in the options collection. |
|field: Comment	|You can automatically connect a comment to all runs started with this setting by setting this field. The comment is put in the run doc for all runs started while the command is active. |
|field: link_mv, link_nv	|These are used by the frontend for detector=tpc only. They simply indicate if the neutron or muon veto are included as part of 'tpc' for this run (for running in combined mode). To the backend this makes no difference. A reader is a reader. To the frontend it can limit the options modes given to the user or help in setting visual cues in the web interface so the operator can figure out what's going on. |
|field: remote    |If this detector is controllable via the API. Set to "true" to disable control from the website and enable control via the API. |

### db.control
The control database is used to propagate commands from the dispatcher to the reader and crate controller nodes. It is used purely internally by the dispatcher. Users wanting to set the DAQ state should set the detector control doc instead (preferably using the web interface). The exception to this is if you're running a small setup with a custom dispatcher and want to issue commands to your readout nodes manually. 
```python
{
    "options_override" : {
        "strax_output_path" : "/strax_output/Run000048"
     },
     "mode" : "two_links",
     "user" : "web",
     "host" : ["fdaq00_reader_0"],
     "acknowledged" : {
         "fdaq00_reader_0" : 0
     },
     "command" : "arm",
     "createdAt": <date object>
}
```

|Field	|Description |
| ----- | ----- |
|options_override	|Override specific options in the options ini document. Mostly used to set the run identifier |
|mode	|Options file to use for this run. Corresponds to the 'name' field of the options doc. |
|user	|Who started the run? Corresponds to the last person to change the detector_status doc during normal operation. Exceptional stop commands can be automatically issued by various subsystems as well in case of errors.
|host	|List of all hosts to which this command is directed. Readers and crate controllers will only process commands addressed to them. |
|acknowledged	|Before attempting to process a command all reader and crate controller processes will first acknowledge the command as received. This does not indicate that processing the command was successful! It just indicates the thing tried. The dispatcher has to watch for the appropriate state change of the slave nodes in order to determine if the command achieved its goal. This is a dictionary, with values set to the timestamp of when the acknowledgement happened. This dictionary must be populated with 0s before insertion. |
|command	|This is the actual command. 'arm' gets the DAQ ready to start. 'start' and 'stop' do what they say on the tin. 'stop' can also be used as a general reset command for a given instance. |

### db.options

This is where the DAQ options are stored. It got its own chapter. Please see [here](daq_options.md).

### db.log

Sometimes the DAQ wants to inform users of some problem or exceptional circumstance.
This is what the log is for.
In the beginning we'll also include lots of debug output.
Cap this collection to something reasonable, probably some hundreds of MB.

The log documents have the following format:
Basic log documents have the following simple format:
```python
{
	"message" : "Received arm command from user web for mode test",
	"priority" : 1,
	"user" : "fdaq00_reader_0",
        "runid": 42
}
```
Where the 'user' is an identifier for which process sent the message, or in case of messages sent by a user it can 
identify the user. The field 'message' is the message itself, and 'priority' is a log level enum. The following table 
gives the standard priorities:

|Priority	|Value	|Use |
| ----- | ----- | ------ |
|0	|DEBUG	|Low-level information. This is only written to disk, it will never show up in the database.|
|1	|MESSAGE	|Normal log output that is important to propagate during normal operation, but does not indicate any exceptional state. |
|2	|WARNING	|Inform the user of an exceptional situation. However this flag is reserved for minor issues that the system will handle on its own and should require no user input. |
|3	|ERROR	|An exceptional situation with major operational impact that requires intervention from the user. |
|4	|FATAL	|An exceptional situation reserved for processes that will likely or certainly cause the calling process to crash. |
|5	|USER	|Users can put messages into the log with the web frontend. |

**Setting a time-to-live (TTL) for debug logs**
Maybe you want to put some debug info into the logs so it's available during normal operation, but maybe we don't necessarily need to store this debug information until the end of time.
You can put (and we did put) a MongoDB TTL index on the log collection that expires documents after one week if they contain the datetime field 'expire_from'.
So if you want your docs to expire make sure to set 'expire_from' to datetime.now() or new Date() or whatever such that they are caught by this index.

Note! Setting this field will cause all documents to expire, not just DEBUG level.
So really only include this field if you're sure the message you're sending will not be interesting for debugging things in the future.

## The Runs Database

The runs database contains collections which may be interesting collaboration-wide by other sub-systems. 

### db.runs

This is the runs database. It is the holy grail, the rosetta stone, and the sacred holy text of the experiment. It contains a directory of every bit of data taken, what settings the data was taken with, and where in the world that data might currently be found. In a production setting great care should be taken when interacting with this database as several systems outside the DAQ also use, change, and depend on it.

Here are the fields set by the DAQ. There are additional fields set at various levels of processing and analysis that are not covered here:

```python
{
    "number": 10000,                                    # run number
    "user": "notch",                                    # who started the run
    "start": ISODate("2018-09-20T13:35:05.642Z"),       # time that the run was started
    "end": ISODate("2018-09-20T13:55:05.642Z"),         # time that the run was ended. d.n.e. if run not ended
    "detectors":  ["tpc", "muon_veto", "neutron_veto"], # subdetectors in run
    "daq_config": {DOCUMENT},                           # the entire options doc used for readout
    "source": {
       "type": "none"                                   # the source type used. (i.e. LED, Rn220). 
    },
    "data": [                                           # all locations where data for this run might be found
        {
            "type":    "live",                          # raw/processed/reduced/etc. live means pre-trigger.
            "host":    "daq",                           # usually host of the machine but 'daq' just means pre-trigger
            "location":    "/live_data/xenonnt/10000"
            "status":  "transferring"                   # transferring/transferred/error
         }
    ],
    "status": "eb_ready_to_upload"                      # A high-level indication of the status of this run
}

```

### db.users, db.shift_rules, db.shifts, etc

There's a bunch of cool stuff implemented in the web frontend that calls on additional collections. However since this isn't pertinent to the readout it isn't documented here.
