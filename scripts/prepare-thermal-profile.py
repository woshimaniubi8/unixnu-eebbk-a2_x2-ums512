#!/usr/bin/env python3
"""Build the A2 SoC thermal model from the pinned OS's J120AP model."""
import argparse
import copy
import hashlib
import json
import plistlib
from pathlib import Path

ORIGINAL_SHA256 = '07d01f6727511d94413dd9214dd471173727486defbb10da7eec4a3a587d3612'
LOCATIONS = ['Unisoc prometheus6 sensor0','Unisoc prometheus6 sensor1','Unisoc prometheus7',
             'Unisoc ank0','Unisoc ank1','Unisoc gpu','Unisoc ank2','Unisoc ank3',
             'Unisoc ank4','Unisoc ank5','Unisoc cputop','Unisoc gpuank2']


def convert(data):
    if hashlib.sha256(data).hexdigest() != ORIGINAL_SHA256:
        raise ValueError('unexpected stock J120AP thermal profile')
    original = plistlib.loads(data)
    result = copy.deepcopy(original)
    sensors = []
    for i, location in enumerate(LOCATIONS):
        sensors.append({'4CharCode': f'TN{i:X}i', 'synthSensor': False,
                        'filterParams': 0, 'matchName': location,
                        'filterParams_INTMIN': True, 'infoOnly': True,
                        'requiresWatchDog': True})
    for i, (location, core) in enumerate([('Unisoc A55 max','coreTypeE'),
                                        ('Unisoc A75 max','coreTypeP'),
                                        ('Unisoc GPU max','coreTypeG')]):
        sensors.append({'4CharCode': f'TN{i}x', 'synthSensor': False,
                        'filterParams': 0, 'matchName': location,
                        'filterParams_INTMIN': False, 'requiresWatchDog': True,
                        'lsSensorType': [core]})
    result['Sensors'] = sensors
    general = result['generalProductConfig']
    for key in ('needsFastDieControl', 'needsLifetimeServo', 'monitorsCameraSensors',
                'needsPushingTSFDtoDisplayDriver', 'shouldDispatchVirtualTemp'):
        general[key] = False
    general['solarContextType'] = 0
    result.pop('lifetimeServoParams', None)
    # Apple PMU/skin/camera estimates are not measurements on this board.
    # Retain the stock die-temperature control table, backed by real maxima.
    result['hotspots'] = [h for h in result['hotspots'] if h['description'] == 'DieTempHS']
    assert len(result['hotspots']) == 1
    result['DecisionTreeTable'] = {k:v for k,v in result['DecisionTreeTable'].items()
                                   if k == 'HotSpotDecisionTableNormal1'}
    return plistlib.dumps(result, fmt=plistlib.FMT_BINARY, sort_keys=False)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('original', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    data = convert(a.original.read_bytes())
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(data)
    readback = plistlib.loads(a.output.read_bytes())
    assert len(readback['Sensors']) == 15
    assert all(s['requiresWatchDog'] for s in readback['Sensors'])
    print(json.dumps({'path':str(a.output),'sha256':hashlib.sha256(data).hexdigest(),
                      'required_sensors':15,'required_mask':'0x7fff',
                      'physical':12,'derived_maxima':3,'battery_pmic_ported':False}))


if __name__ == '__main__':
    main()
