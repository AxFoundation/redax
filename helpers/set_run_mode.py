import os
import pymongo
from pymongo import MongoClient


pswd = os.environ['ATLAS_ADMIN_PASSWORD'] 
path = os.environ['ATLAS_LINK'] 
client = MongoClient(f'mongodb+srv://admin:{pswd}{path}')

db = client['daq']
collection = db['options']

doc = {
    "name": "triggerless_test",
    "user": "elykov",
    "source": "none",
    "description": "Test config for a triggerless readout of V1725SB boards",
    "detector" : "Test detector",
    "run_start":0,
    "strax_chunk_overlap": 0.5,
    "strax_header_size": 31,
    "strax_output_path": "/home/alex/raw",
    "strax_chunk_length": 21,
    "strax_fragment_length": 220,
    "baseline_dac_mode": "fit",
    "baseline_value": 16000,
    "firmware_version": 1,
    "compressor": "lz4",
    "boards":
    [
        {"crate": 0, "link": 4, "board": 165,
            "vme_address": "FFFF0000", "type": "V1725", "host": "fdaq0_reader"},
    ],
    "registers" : [
		{
			"comment" : "board reset register",
			"board" : -1,
			"reg" : "EF24",
			"val" : "0"
		},
		{
			"comment" : "events per BLT-originally it was 1",
			"board" : -1,
			"reg" : "EF1C",
			"val" : "1"
		},
		{
			"comment" : "Front Panel Trigger Out Enable Mask",
			"board" : -1,
			"reg" : "8110",
			"val" : "00000000"
		},
		{
			"comment" : "for  BUSY on TRG_OUT, LVDS new management, General Purpose I/O, LVDS[15-4] outputs, LVDS[3-0] inputs, TRG/CLK at TTL level (139) or NIM level (138)",
			"board" : -1,
			"reg" : "811C",
			"val" : "D0138"
		},
		{
			"comment" : "BERR register, 10=enable BERR",
			"board" : -1,
			"reg" : "EF00",
			"val" : "10"
		},
		{
			"comment" : "Trigger logic: 80000000 is software only, software + external C0000000",
			"board" : -1,
			"reg" : "810C",
			"val" : "C0000000"
		},
		{
			"comment" : "Post Trigger (time between trigger and end of time window). 80 for 2.5 us",
			"board" : -1,
			"reg" : "8114",
			"val" : "80"
		},
		{
			"comment" : "Channel enable mask. FF= all channels on",
			"board" : -1,
			"reg" : "8120",
			"val" : "FF"
		},
		{
			"comment" : "Buffer organization register. A for new FW.",
			"board" : -1,
			"reg" : "800C",
			"val" : "A"
		},
		{
			"comment" : "DAC default configuration. 1000 = neg unipolar + offset, 1000",
			"board" : -1,
			"reg" : "8098",
			"val" : "1000"
		},
		{
			"comment" : "50: NO ZS + Falling + Sequential + External signal + Non Overlap Triggers, 20050 same with ZLE",
			"board" : -1,
			"reg" : "8000",
			"val" : "50"
		},
		{
			"comment" : "Event size register. Required for new FW. Words.",
			"board" : -1,
			"reg" : "8020",
			"val" : "0"
		},
	],
    "channels":{"165":[0, 1, 2, 3, 4, 5, 6, 7]},
}

if collection.find_one({"name": doc["name"]}) is not None:
    print(f'These options already exist. Please provide a unique name for the options doc!')
else:
    try:
        collection.insert_one(doc)
        print(f'Inserting a run doc with mode : {doc["name"]}')
    except Exception as e:
        print(f'Insert failed. Maybe your JSON is bad. Error follows:')
        print(e)
