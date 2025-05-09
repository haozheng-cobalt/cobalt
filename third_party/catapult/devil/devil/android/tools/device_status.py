#!/usr/bin/env python
# Copyright 2016 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""A script to keep track of devices across builds and report state."""

import argparse
import json
import logging
import os
import re
import sys

if __name__ == '__main__':
  sys.path.append(
      os.path.abspath(
          os.path.join(os.path.dirname(__file__), '..', '..', '..')))
from devil.android import battery_utils
from devil.android import device_denylist
from devil.android import device_errors
from devil.android import device_list
from devil.android import device_utils
from devil.android.sdk import adb_wrapper
from devil.android.tools import script_common
from devil.constants import exit_codes
from devil.utils import logging_common
from devil.utils import lsusb

logger = logging.getLogger(__name__)

_RE_DEVICE_ID = re.compile(r'Device ID = (\d+)')


def IsDenylisted(serial, denylist):
  return denylist and serial in denylist.Read()


def _BatteryStatus(device, denylist):
  battery_info = {}
  try:
    battery = battery_utils.BatteryUtils(device)
    battery_info = battery.GetBatteryInfo(timeout=5)
    battery_level = int(battery_info.get('level', 100))

    if battery_level < 15:
      logger.error('Critically low battery level (%d)', battery_level)
      battery = battery_utils.BatteryUtils(device)
      if not battery.GetCharging():
        battery.SetCharging(True)
      if denylist:
        denylist.Extend([device.adb.GetDeviceSerial()], reason='low_battery')

  except (device_errors.CommandFailedError,
          device_errors.DeviceUnreachableError):
    logger.exception('Failed to get battery information for %s', str(device))

  return battery_info


def DeviceStatus(devices, denylist):
  """Generates status information for the given devices.

  Args:
    devices: The devices to generate status for.
    denylist: The current device denylist.
  Returns:
    A dict of the following form:
    {
      '<serial>': {
        'serial': '<serial>',
        'adb_status': str,
        'usb_status': bool,
        'denylisted': bool,
        # only if the device is connected and not denylisted
        'type': ro.build.product,
        'build': ro.build.id,
        'build_detail': ro.build.fingerprint,
        'battery': {
          ...
        },
        'imei_slice': str,
        'wifi_ip': str,
      },
      ...
    }
  """
  adb_devices = {
      a[0].GetDeviceSerial(): a
      for a in adb_wrapper.AdbWrapper.Devices(
          desired_state=None, long_list=True)
  }
  usb_devices = set(lsusb.get_android_devices())

  def denylisting_device_status(device):
    serial = device.adb.GetDeviceSerial()
    adb_status = (adb_devices[serial][1]
                  if serial in adb_devices else 'missing')
    usb_status = bool(serial in usb_devices)

    device_status = {
        'serial': serial,
        'adb_status': adb_status,
        'usb_status': usb_status,
    }

    if not IsDenylisted(serial, denylist):
      if adb_status == 'device':
        try:
          build_product = device.build_product
          build_id = device.build_id
          build_fingerprint = device.build_fingerprint
          build_description = device.build_description
          wifi_ip = device.GetProp('dhcp.wlan0.ipaddress')
          battery_info = _BatteryStatus(device, denylist)
          try:
            imei_slice = device.GetIMEI()
          except device_errors.CommandFailedError:
            logging.exception('Unable to fetch IMEI for %s.', str(device))
            imei_slice = 'unknown'

          if (device.product_name == 'mantaray'
              and battery_info.get('AC powered', None) != 'true'):
            logger.error('Mantaray device not connected to AC power.')

          device_status.update({
              'ro.build.product': build_product,
              'ro.build.id': build_id,
              'ro.build.fingerprint': build_fingerprint,
              'ro.build.description': build_description,
              'battery': battery_info,
              'imei_slice': imei_slice,
              'wifi_ip': wifi_ip,
          })

        except (device_errors.CommandFailedError,
                device_errors.DeviceUnreachableError):
          logger.exception('Failure while getting device status for %s.',
                           str(device))
          if denylist:
            denylist.Extend([serial], reason='status_check_failure')

        except device_errors.CommandTimeoutError:
          logger.exception('Timeout while getting device status for %s.',
                           str(device))
          if denylist:
            denylist.Extend([serial], reason='status_check_timeout')

      elif denylist:
        denylist.Extend([serial],
                        reason=adb_status if usb_status else 'offline')

    device_status['denylisted'] = IsDenylisted(serial, denylist)

    return device_status

  parallel_devices = device_utils.DeviceUtils.parallel(devices)
  statuses = parallel_devices.pMap(denylisting_device_status).pGet(None)
  return statuses


def _LogStatuses(statuses):
  # Log the state of all devices.
  for status in statuses:
    logger.info(status['serial'])
    adb_status = status.get('adb_status')
    denylisted = status.get('denylisted')
    logger.info('  USB status: %s',
                'online' if status.get('usb_status') else 'offline')
    logger.info('  ADB status: %s', adb_status)
    logger.info('  Denylisted: %s', str(denylisted))
    if adb_status == 'device' and not denylisted:
      logger.info('  Device type: %s', status.get('ro.build.product'))
      logger.info('  OS build: %s', status.get('ro.build.id'))
      logger.info('  OS build fingerprint: %s',
                  status.get('ro.build.fingerprint'))
      logger.info('  Battery state:')
      for k, v in status.get('battery', {}).items():
        logger.info('    %s: %s', k, v)
      logger.info('  IMEI slice: %s', status.get('imei_slice'))
      logger.info('  WiFi IP: %s', status.get('wifi_ip'))


def _WriteBuildbotFile(file_path, statuses):
  buildbot_path, _ = os.path.split(file_path)
  if os.path.exists(buildbot_path):
    with open(file_path, 'w') as f:
      for status in statuses:
        try:
          if status['adb_status'] == 'device':
            f.write(
                '{serial} {adb_status} {build_product} {build_id} '
                '{temperature:.1f}C {level}%\n'.format(
                    serial=status['serial'],
                    adb_status=status['adb_status'],
                    build_product=status['type'],
                    build_id=status['build'],
                    temperature=float(status['battery']['temperature']) / 10,
                    level=status['battery']['level']))
          elif status.get('usb_status', False):
            f.write('{serial} {adb_status}\n'.format(
                serial=status['serial'], adb_status=status['adb_status']))
          else:
            f.write('{serial} offline\n'.format(serial=status['serial']))
        except Exception:  # pylint: disable=broad-except
          pass


def GetExpectedDevices(known_devices_files):
  expected_devices = set()
  try:
    for path in known_devices_files:
      if os.path.exists(path):
        expected_devices.update(device_list.GetPersistentDeviceList(path))
      else:
        logger.warning('Could not find known devices file: %s', path)
  except IOError:
    logger.warning('Problem reading %s, skipping.', path)

  logger.info('Expected devices:')
  for device in expected_devices:
    logger.info('  %s', device)
  return expected_devices


def AddArguments(parser):
  parser.add_argument(
      '--json-output', help='Output JSON information into a specified file.')
  parser.add_argument('--denylist-file', help='Device denylist JSON file.')
  parser.add_argument(
      '--known-devices-file',
      action='append',
      default=[],
      dest='known_devices_files',
      help='Path to known device lists.')
  parser.add_argument(
      '--buildbot-path',
      '-b',
      default='/home/chrome-bot/.adb_device_info',
      help='Absolute path to buildbot file location')
  parser.add_argument(
      '-w',
      '--overwrite-known-devices-files',
      action='store_true',
      help='If set, overwrites known devices files wiht new '
      'values.')


def main():
  parser = argparse.ArgumentParser()
  logging_common.AddLoggingArguments(parser)
  script_common.AddEnvironmentArguments(parser)
  AddArguments(parser)
  args = parser.parse_args()

  logging_common.InitializeLogging(args)
  script_common.InitializeEnvironment(args)

  denylist = (device_denylist.Denylist(args.denylist_file)
              if args.denylist_file else None)

  expected_devices = GetExpectedDevices(args.known_devices_files)
  usb_devices = set(lsusb.get_android_devices())
  devices = [
      device_utils.DeviceUtils(s) for s in expected_devices.union(usb_devices)
  ]

  statuses = DeviceStatus(devices, denylist)

  # Log the state of all devices.
  _LogStatuses(statuses)

  # Update the last devices file(s).
  if args.overwrite_known_devices_files:
    for path in args.known_devices_files:
      device_list.WritePersistentDeviceList(
          path, [status['serial'] for status in statuses])

  # Write device info to file for buildbot info display.
  _WriteBuildbotFile(args.buildbot_path, statuses)

  # Dump the device statuses to JSON.
  if args.json_output:
    with open(args.json_output, 'w') as f:
      f.write(
          json.dumps(
              statuses, indent=4, sort_keys=True, separators=(',', ': ')))

  live_devices = [
      status['serial'] for status in statuses
      if (status['adb_status'] == 'device'
          and not IsDenylisted(status['serial'], denylist))
  ]

  # If all devices failed, or if there are no devices, it's an infra error.
  if not live_devices:
    logger.error('No available devices.')
  return 0 if live_devices else exit_codes.INFRA


if __name__ == '__main__':
  sys.exit(main())
