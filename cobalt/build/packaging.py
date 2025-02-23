#!/usr/bin/env python3
"""Packages Cobalt with a given layout."""

import argparse
import os
import shutil
import tempfile


def copy(src_file_path, dst_file_path):
  os.makedirs(os.path.dirname(dst_file_path), exist_ok=True)
  shutil.copy2(src_file_path, dst_file_path)


def run(lay_out):
  parser = argparse.ArgumentParser()
  parser.add_argument('--name', required=True, help='of archive and base dir')
  parser.add_argument('out_dir', help='generated by GN and built by Ninja')
  parser.add_argument('package_dir', help='where to place the archive')
  args = parser.parse_args()

  with tempfile.TemporaryDirectory() as tmp_dir:
    base_dir = os.path.join(tmp_dir, args.name)
    os.makedirs(base_dir)
    lay_out(args.out_dir, base_dir)
    shutil.make_archive(
        os.path.join(args.package_dir, args.name), 'zip', tmp_dir)
