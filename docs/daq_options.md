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

# DAQ Options Reference

DAQ options are stored in MongoDB documents in the daq.options collection.
Colloqially a single set of options may be referred to as a 'run mode'.
Example run modes would be "background_stable" or "rn220_heveto". This is the mode a shifter will choose when starting the DAQ.

This section provides all settings and their function.

## Organizational Fields

The following fields are required for organizational purposes:

| Field | Description |
| ---- | ----- |
| name | The name of this mode. Must be unique. The web interface will ensure that this is unique but if you're not using nodiaq you have to ensure this yourself. |
| user | The user who created the mode. |
| description | A human readable description of the mode (like a sentence) to be displayed on the frontend |
| detector | Which detector does this mode apply to. For XENONnT we use 'tpc', 'muon_veto', 'neutron_veto', or for modes that are meant to be included by top-level modes, 'include'. This could even be a list for configs intended for linked mode. |

Note that only 'name' is actually used by redax. The other options are used by the web frontend and only included here for completion.

## The 'includes' field

Sometimes we re-use configuration between many run modes.
For example, the electronics are defined once and, provided the physical cabling doesn't change, this definition is valid for all run modes for the entire experiment.
So it doesn't make sense to copy-paste this definition into each and every mode document.
This is where 'includes' come in.

Include documents are given detector 'include', which is simply to ensure they don't appear in the mode list for a shifter trying to start a run for that detector, since they are by definition incomplete.
They can then be included by appending the name of the include document to the 'includes' field of the parent doc. 

For example the document 'background_stable' might look like:
```python
{
    "name": "background_stable",
    "includes": ["tpc_channel_definition", "muon_veto_channel_definition", "neutron_veto_channel_definition",
                "default_registers_tpc", "default_registers_muon_veto", "default_registers_neutron_veto", 
                "master_channel_map", "strax_output_ceph"],
    # ALL OTHER OPTIONS
    #
    # ...
    # 
    #
}
```

This will then include within this run mode all options found in each options document named in the 'include' array.
In case of repeated options, the highest level document should override the others.
However *it is highly recommended to not override options declared in included documents!*
This may lead to undefined behavior, especially in case of complex, nested sub-objects where only certain fields get overridden.

## Electronics Definitions

Electronics are defined in the 'boards' field as follows:

```python
"boards": [
    {
      "board": 100,
      "crate": 0,
      "vme_address": "0",
      "type": "V1724",
      "link": 0,
      "host": "daq0_reader_0"
    },
    {
      "board": 101,
      "crate": 0,
      "vme_address": "0",
      "type": "V1724",
      "link": 0,
      "host": "daq1_reader_0"
    },
    {
      "board": 102,
      "crate": 0,
      "vme_address": "0",
      "type": "V1724",
      "link": 2,
      "host": "daq1_reader_1"
    },
    {
      "vme_address": "0",
      "board": 1000,
      "crate": 0,
      "link": 1,
      "host": "daq0_controller_0",
      "type": "V2718"
    },
    {
      "vme_address": "44440000",
      "board": 145,
      "crate": -1,
      "link": -1,
      "host": "daq0_controller_0",
      "type": "V1495"
    }
  ]
```

This is an example that might be used for reading out 3 boards and defining one crate controller with V1495 in the electronics setup given in the [previous chapter](installation.md).
Each subdocument contains the following:

| Option | Description |
| ----- | --------- |
| board | A unique identifier for this board. The best thing is to just use the digitizer serial number. |
| crate | The 'crate' as defined in CAEN lingo. Namely, if multiple boards are connected to one optical link they get crate numbers zero to seven (max) defining their order in the daisy chain. The first board in the daisy chain is zero. The order is defined by the direction of the optical link propagation, which you can deduce by the little lights on the board that light when they receive an input. |
| vme_address | It is planned to support readout via a V2718 crate controller over the VME backplane. In this case board addressing is via VME address only and crate would refer to the location of the crate controller in the daisy chain. This feature is not yet implemented so the option is placeholder (but must be included). |
| link | Defines the optical link index this board is connected to. This is simple in case of one optical link, though like plugging in USB-A there's always a 50-50 chance to guesss it backwards. It becomes a bit more complicated when you include multiple A3818s on one server. There's a good diagram in CAEN's A3818 documentation. |
| host | This is the DAQ name of the process that should control the board. Multiple processes cannot share one optical link (but one process can control one optical link). |
| type | Either V1724, V1724_MV, or V1730 for digitizers, V2718 for crate controllers, or V1495 for the FGPA. If more board types are supported they will be added. |

Note that the "crate" and "link" fields for the V1495 don't have meaning and can take any value, but its host should match that of the V2718.

## Register Definitions

Redax provides direct control of CAEN registers and does not wrap or otherwise simplify this functionality. Registers are loaded into digitizers in the order they are provided here.

Here is an example:
```python
"registers": [
    {
      "val": "1",
      "reg": "EF24",
      "board": "all"
    },
    {
      "val": "1",
      "reg": "EF1C",
      "board": "tpc"
    },
    {
      "val": "1",
      "reg": "801C",
      "board": 1337
    },
    ...
]
```

| Field | Description |
| ----- | ----------- |
| reg | String. The register to write to. **Hex**. |
| val | String. The value to write. **Hex**. |
| board | Varies. If "all", the write will go to all boards. If a different string like "tpc", will go to all boards servicing the specified detector. If an int, will go to the board with that identifier. |

## V2718

The V2718 crate controller has a few options to configure. Note that they must be a nested object with detector names as keys:
```python
"V2718": {
  "tpc" : {
    "pulser_freq": 0,
    "neutron_veto": 0,
    "muon_veto": 0,
    "led_trigger": 0,
    "s_in": 1
  },
  "muon_veto": {
    ...
  }
}
```

| Field | Description |
| ------ | ----------- |
| pulser_freq | Float. The frequency to pulse the trigger/LED pulser in Hz. Supports from <1 Hz up to some MHz. Keep in mind this may not be implemented exactly since the CAEN API doesn't support every possible frequency exactly, but the software will attempt to match the desired frequency as closely as possible. |
| neutron_veto | Should the S-IN signal be propogated to the neutron veto (connector 4)? 1-yes, 0-no |
| muon_veto | Should the S-IN signal be propogated to the muon veto (connector 1)? 1-yes, 0-no |
| led_trigger | Should the LED pulse be propagated to the LED driver (connector 2)? 1-yes, 0-no |
| s_in | Should the run be started with S-IN (connector 0)? 1-yes, 0-no |

The top-level field 'run_start' (next section) is also required to define run start via S-IN.

## Software Configuration Options

Various options that tell redax how to run.

```python
{
    "run_start": 1,
    "baseline_dac_mode": "cached",
    "baseline_reference_run": 1976,
    "baseline_value": 16000,
    "baseline_fixed_value": 4000,
    "baseline_fallback_mode": "fail",
    "processing_threads": {
      "reader0_reader_0": 2,
      "reader1_reader_1": 6,
     },
    "detectors" {
      "reader0_reader_0": "tpc",
      "reader5_reader_0": "muon_veto"
    }
}
```

| Option | Description |
| -------- | ---------- |
| run_start | Tells the DAQ whether to start the run via register or S-in. 0 for register, 1 for S-in. Note that starting by register means that the digitizer clocks will not be synchronized. This can be fine if you run with an external trigger and use the trigger time as synchronization signal. If running in triggerless mode you need to run with '1' and have your hardware set up accordingly. |
| baseline_dac_mode | cached/fixed/fit. This defines how the DAC-offset values per channel are set. If set to "cached" the program will load cached baselines from the run specified in *baseline_reference_run*. If it can't find that run it will fall back to the value in *baseline_fixed_value*. If set to "fixed" it will use *baseline_fixed_value* in any case. If set to 'fit' it will attempt to adjust the DAC offset values until the baseline for each channel matches the value in *baseline_value*. If using negative voltage signals the default value of 16000 is a good one. Baselines for each run are cached in the *dac_values* collection of the daq database. Note that this field can be a dictionary, with keys specifying detectors and values what is given above, for instance `{"tpc": "fit", "neutron_veto": "cachd"}` |
| baseline_fallback_mode | cached/fixed/fail. Sometimes the baseline fitting routine doesn't converge. This option determines what happens in this case (only the non-converging channels are affected). Default is 'fail'. |
| baseline_reference_run | Int. If either 'baseline_dac_mode' or 'baseline_fallback_mode' are set to 'cached' it will use the values from the run number defined here. Nesting is supported as for `baseline_dac_mode`. |
| baseline_value | Int. If 'baseline_dac_mode' is set to 'fit' it will attempt to adjust the baselines until they hit the decimal value defined here, which must lie between 0 and 16385 for a 14-bit ADC. Default 16000. |
| baseline_fixed_value | Int. Use this to set the DAC offset register directly with this value. See CAEN documentation for more details. Default 7000. |
| processing_threads | Dict. The number of threads working on converting data between CAEN and strax format. Should be larger for processes responsible for more boards and can be smaller for processes only reading a few boards. For example, 24 threads will very easily handle a data flow of 200 MB/s (uncompressed) through that instance, but if you aren't expecting that much data then smaller values are fine. The default value is 8, but not specifying this could cause issues with processing. |
| detectors | Dict. Which detector a given instance is attached to. Used mainly in aggregating registers. Required |

## Strax Output Options

There are various configuration options for the strax output that must be set. 

```python
{
  "strax_chunk_overlap": 0.5,
  "strax_output_path": "/data/xenon/raw",
  "strax_chunk_length": 5.0,
  "strax_fragment_payload_bytes": 220,
  "strax_buffer_num_chunks": 2,
  "strax_chunk_phase_limit": 1,
  "compressor": "lz4"
}
```

| Option | Description |
| ---- | ---- |
| strax_chunk_overlap | Float. Defines the overlap period between strax chunks in seconds. Make is at least some few times larger than your typical event length. In any case it should be larger than your largest expected event. Default 0.5. |
| strax_chunk_length | Float. Length of each strax chunk in seconds. There's some balance required here. It should be short enough that strax can process reasonably online, as it waits for each chunk to finish then loads it at once (the size should be digestable). But it shouldn't be so short that it needlessly micro-segments the data. Somewhere vaguely around 10 seconds seems reasonable, in production we use anything between 5 and 21. Default 5. |
| strax_fragment_payload_bytes | Int. How long are the fragments? In general this should be long enough that it definitely covers the vast majority of your SPE pulses. Our SPE pulses are ~100 samples, so the default value of 220 bytes (2 bytes per sample) provides a small amount of overhead. Undefined behavior if the value is odd, but expect segfaults. |
| strax_output_path | String. Where should we write data? This must be a locally mounted data store. Redax will handle sub-directories so just provide the top-level directory where all the live data should go (e.g. `/data/live`). |
| strax_buffer_num_chunks | Int. How many full chunks should get buffered before writing? Setting this at 1 or lower may cause data loss, and greater than 2 usually means you need more memory in your readout machine. A value of 2 means that if chunks 5 and 6 are buffered, as soon as something in chunk 7 shows up, chunk 5 is dumped to disk. |
| strax_chunk_phase_limit | Int. Sometimes pulses will show up at the processing stage late (or somehow behind the rest of them). If a pulse is this many chunks behind (or out of phase with) the chunks currently being buffered, log a warning to the database. |
| compressor | lz4/blosc/none/delete. Which algorithm to use in compressing the chunks. LZ4 is squeezier but blosc is slightly faster. Blosc does have an issue where the size must fit into 31 bits, but you have to be running long and hard to hit this. "none" and "delete" are self-explanatory. Default lz4 |

## Channel Map

Redax needs to provide the channel values to strax. Therefore the channel map (mapping module/channel in the digitizers to PMT position) must be provided at the readout stage.

This is in a quite simple format: 

```python
"channels": {
    "110": [
      78,
      184,
      219,
      227,
      235,
      205,
      243,
      209
    ],
    "117": [
      174,
      173,
      182,
      98,
      187,
      97,
      113,
      123
    ]
    ...
}
```

Here 'channels' is a dictionary where each key is the string value of a board's ID number as defined in the 'boards' field (typically the serial number). Each value is an array of 8 values (or 16, if appropriate) giving the PMT position of each channel of that digitizer. This PMT position absolutely must match the PMT map assumed by the data processor.

Note that if there are any skipped channels (for instance, if you are using input channels 0, 1, and 3, but not 2), a "blank" or placeholder value should be inserted.

## Trigger thresholds

Redax assigns trigger thresholds using a syntax identical to that of the channel map (above). The field is called `thresholds`.

## Lower-level diagnostic options

Redax accepts a variety of options that control various low-level operations. The default values should be fine, and really should only be adjusted if you know what's going on or don't mind dealing with strange behavior.

| Option | Description |
| ---- | ---- |
| baseline_max_steps | Int. The maximum number of steps during baselining. Steps involve measuring the baseline and trying to adjust it towards the target value. Default 30. |
| baseline_adjustment_threshold | Int. How close the measured baseline must be to the target baseline in ADC units. If the absolute difference is less than this value, a channel is considered to have converged. Default 10. |
| baselie_convergence_threshold | Int. How many consecutive times a channel must be within the adjustment threshold to be considered stable and finished. Default 3. |
| baseline_min_adjustment | Int. The minimum change to the DAC value, given in DAC units. Default 12 (approx 3 ADC units). |
| baseline_rebin_log2 | Int. How much to rebin samples by when calculating the baseline. This is intended to provide some level of noise immunity. Samples are bit-shifted right by this value (ie, sample >> value). Default 1. |
| baseline_bins_around_max | Int. How close around the mode the majority of samples must be. This is given in units of rebinned samples, so if this value is 3 and the rebin value is 1, then the region around the max is 6 ADC samples (3 << 1) in each direction. Default 3 |
| baseline_fraction_around_max | Float. What fraction of total samples in the pulse must be around the mode for the pulse to be accepted. Default 0.8. |
| baseline_triggers_per_step | Int. How many software triggers to send for each baseline step. Default 3. |
| baseline_ms_between_triggers | Int. How long between software triggers. Default 10. |
| baseline_max_dac | Int. The maximum value you want a digitizer to write to a DAC channel. Default 65535 |
| baseline_min_dac | Int. The minimum value you want a digitizer to write to a DAC channel. Default 0 |
| baseline_adc_to_dac | Float. The scale factor that says how large an adjustment of the DAC to make, given a measured difference of X ADC units. Default -3. |
| baseline_dac_start | Int. The starting DAC value for the baseline fitting sequence. Closer to the final value is better, but too close might cause some issues. Default 10000. |
| do_sn_check | 0/1. Whether or not to have each board check its serial number during initialization. Default 0. |
| us_between_reads | Int. How many microseconds to sleep between polling digitizers for data. This has a major performance impact that will matter when under extremely high loads (ie, the bleeding edge of what your server(s) and boards are capable of), but otherwise shouldn't matter much. Default 10. |
| transfer_batch | Int. After how many readout loops do you send accumulated data packets out for processing? Higher values mean less time waiting for mutexes, but too high could lead to problems at the processing stage, especially if rates are very low. Default 8. |
| mv_time_offset | Int. A constant value to subtract from all timestamps recorded by V1724_MV boards. Default 2420. |

