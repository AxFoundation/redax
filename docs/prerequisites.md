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

# Installation of Prerequisites

This section lists all the prerequisites needed. We're assuming an Ubuntu 18.04 LTS system.

## Hardware notes

The most basic system will consist of a CAEN V1724 connected via optical link to a CAEN A3818 or A2818 PCI(e) card installed in the same PC where the software will run. More complex setups, for example using a V2718 crate controller to facilitate synchronized starting of multiple V1724, are of course also possible.

**Note:** the V1724 can be either used with the DPP_DAW firmware or with the default firmware without 'zero-length-encoding' enabled. ZLE support may be included in a future release if it is needed.

## Libraries from the package repo

  * [LZ4](http://lz4.org) is needed as the primary compression algorithm. Note that redax was developed against a particularly antiquated version of this library, so if you get a newer one from github then shenanigans might ensue.
  * [Blosc](http://blosc.org/) is the secondary for compression algorithm.
  * Normal build libraries required. Support for C++17 is required.

Install with: `sudo apt-get install build-essential libblosc-dev liblz4`

## CAEN Libraries

  * CAENVMElib v2.5+
  * Driver for your CAEN PCI card

Both of these are available from [CAEN](http://www.caen.it) directly. We also maintain a private repository in the XENON1T organization called daq_dependencies with the production versions of all drivers and firmwares. 


## MongoDB CXX Driver

This is a condensation of the instructions found [here](https://mongodb.github.io/mongo-cxx-driver/mongocxx-v3/installation). If in doubt, follow those instructions rather than these.

### Step 1: Build mongo C driver
The CXX driver depends on the C driver now.

1. Get with: `wget https://github.com/mongodb/mongo-c-driver/releases/download/1.9.2/mongo-c-driver-1.9.2.tar.gz` (or whichever version you choose)
2. untar `tar -xvzf mongo-c-driver-1.9.2.tar.gz`
3. See instructions [here](http://mongoc.org/libmongoc/current/installing.html). We're not gonna mess with the package repo versions but will compile from source.
4. prerequisites: `sudo apt-get install pkg-config libssl-dev libsasl2-dev`
5. `./configure –disable-automatic-init-and-cleanup`. If you get a version newer than 1.9.2 you will probably need to use cmake here rather than configure. Follow Mongo's documentation if there's any uncertainty.
6. `make -j && sudo make install`

That's it! It worked perfectly the first time when we did it on a fresh system.

### Step 2: Build the cxx driver

1. get code: `git clone https://github.com/mongodb/mongo-cxx-driver.git –branch releases/stable –depth 1` Note this gets the newest stable release. For deployment might want to fix a version and update only at fixed times.
2. `cd mongo-cxx-driver/build`
3. `cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local ..` The docs make special note not to forget the trailing '..'
4. Install polyfill (just in case?) `sudo make -j EP_mnmlstc_core`
5. `make -j && sudo make install`

That's it.

## Installing MongoDB Server

The previous step installed the mongodb C++ driver onto your machine. The driver allows you to interact with a mongodb 
deployment sitting anywhere provided you have the proper credentials. However, it doesn't actually create a database. If 
you need to install a database there are a few options.

1. Use a cloud-hosted DB. [Mlab](https://www.mlab.com) and [MongoDB Atlas](https://www.mongodb.com/cloud/atlas) are popular choices and feature a free tier, which is enough for testing. Note that MLab has been acquired by MongoDB.
2. Use your running cloud service. If you happen to be XENONnT we use a mongo cloud deployment that allows us to deploy new databases to our own servers at the click of a button. It is a service from MongoDB and costs a fee per data-bearing machine. Our production systems are managed in this way.
3. Install your own standalone database. This is easy to do and gives you full freedom to use your own hardware. Additionally, a cloud-based solution may not be fully appropriate for a DAQ deployment that is inexorably tied to specific physical hardware (i.e. the detector and electronics readout).

### Local Installation

The MongoDB documentation for installing a standalone database is really good and you should follow it. Don't forget to enable authentication.

## Anaconda Environment for the dispatcher and system monitor

If you use the dispatcher, system monitor, and API functionalities you need python3 and some libraries. If you're installing this on a test or shared system you might consider installing everything in an [anaconda](https://www.anaconda.com/) environment.

Here are the required packages via pip (you may need to install it):
```
pip install psutil pymongo 
```

