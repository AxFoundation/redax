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

# Hardware-free DAQ

Redax comes with a simple waveform simulator.

## Wait, what?

Some issues only arise when you're passing a lot of data around between a lot of threads.
You might have one or two spare digitizers sitting around, but you may not have enough PMTs or a running TPC you can use to test things.
You can hook up your board(s) to an external pulser, but a constant external trigger frequency won't always test everything you want.
Also, adding new boards usually means a trip to the lab, which may or may not be close to your desk, and you'll probably go past the coffee machine on your way, and before you realize it, it's lunchtime.
Far simpler to have a testing solution that doesn't require to you to leave your office.

## The caveats

The redax waveform simulator is bare-bones.
The data it produces should be written straight to /dev/null (but can't, because you can't make subfolders there).
It takes CPU cycles away from the rest of the program that's trying to reformat the data for output.
It was envisioned as a method to test new features by assuming digitizers with "perfect" performance.

## What it assumes

The redax waveform simulator assumes you're using V1724 boards and a "small" fake TPC with the usual aspect ratio.
The PMTs are arranged in two identical arrays of hexagonal rings.
PMT0 is in the center of the top array, and PMTs are incrementally numbered in counter-clockwise oriented rings beginning from the positive x-axis.
The bottom array is identical with an offset in the numbering.

## How it works, "physics"-y

All coordinates are assumed to be in units of PMTs, so a depth of 3.5 means 3.5 PMTs.
First, redax generates an event location.
This is flat in z with a minimum depth of 0.5 to prevent overlap between the S1 and S2, and also flat in theta and r.
Note that it is not flat in r-squared.
Next, redax generates an S1 between 11 and 30 PE.
Then, it assumes the S2/S1 gain is normally distributed around 100 with a standard deviation of 20 and electron loss in proportion to the depth and electron absorbtion length, which it uses to generate the S2 from the S1.
Now that we have coordinates and photon numbers, the hitpattern is generated.
The S1 top fraction is 0.1 at the bottom and 0.4 at the top, and linear.
The S2 top fraction is 0.65, and the S2 hitpattern is Gaussian with a width of 1.3 PMTs.
S1s are 40ns wide, and S2 width is 1us at the top and increases with 200ns/sqrt(z).
PMT numbers and hit times are randomly selected, given these weights and widths.
These values are then used to generate PMT waveforms, which are converted to the digitizer's internal format and made ready for "readout".

## Implementation details

The first three steps (location, size, and hitpattern) are performed in a static thread, which sleeps both between the S1 and S2, and between the S2 and the next event.
This makes pulses show up in the digitizers at quasi-realistic intervals.
Once hits (PMT id and time) are generated, these are sorted out into digitizer-specific parts and sent to each digitizer.
Each digitizer has its own thread that is uses to convert PMT id's and times into waveforms, for which it takes a single photon model and randomly generates a scale factor (normally distributed about 1 with a width of 0.15).
This single photon model is characteristic of an R11410.
This is then converted into the expected format and added to its internal buffer.
When the main readout thread "reads" from the digitizer, it just takes whatever contents are in this buffer (technically it takes the whole buffer via std::move).
From this point, the fax pulses are "indistinguishable" from real pulses and exhibit all the usual digitizer features like saturation and clock rollovers (though rollovers are tracked differently because asynchronous event generation).

## Known limitations

The zero-padding pre- and post-threshold crossing isn't implemented, so pulses are rather shorter than from hardware digitizers.
Dark counts aren't a thing, and probably can't be added without an overhaul of how the internal buffer works.
This is due to how overlaps are treated, which is to say that they aren't.
I haven't looked at the sum waveform or hitpattern of any pulse, and I don't particularly intend to.
The waveform simulator requires an overhead of (n_boards + 1) threads, so if you want to simulate high rates the processing performance won't be strictly representative, unless you have an absurd number of threads on your readout machine.
There are no electron trains (not sure if this is a limitation or a feature).

## How to use

The waveform simulator is great for testing redax features or a new deployment, without having to worry about hardware.

### f1724

The fax digitizer has the model number "f1724", because it's a fake V1724.
Beyond this, the "link" and "crate" fields behave the same as in hardware.
The "helpers/make_fax_config.py" script will generate all fax-specific options necessary,
but before running it you should change things like hostnames, database names, output directories, etc.
The script takes a handful of options:

|Argument   |Description   | Default value  |
| ----- | ----- | ----- |
|--size | How many rings of PMTs the detector has, i.e. 3 = the center PMT and 3 additional full rings. | 2 |
|--rate | The rate of events, in Hz. Will be generated Poisson-ly. | 10 |
|--e-lifetime | Electron absorbtion length, in units of TPC lengths, i.e. S2_actual = S2_real \* exp(z\*value) | 1.5 |
|--drift-speed | Drift speed of electrons in units of PMTs/ns. | 1e-4 |
|--name | The name of your option document | - |

This script will divide boards as evenly as possible among as few links as are necessary to support the number of boards, so 7 boards will all go on one link, and 9 boards will end up with 4 on one link and 5 on another.
PMTs are assigned as discussed above.
As there is no "functional" f2718 (its function in the broader scope is served by its mere existence), the only way to start the process is by software, rather than an S-IN signal, so make sure that the config is set appropriately.
There is only one thread that relies on real-time, so there is no impact on the synchronization of the "digitizers".

