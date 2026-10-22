"""ADI_Logger: the Live-side half of tools/interaction_logger.

Pick it in Live's Preferences > Link, Tempo & MIDI > Control Surface, with
Input and Output set to None. See live_logger.py for what it records.
"""
from __future__ import absolute_import, print_function, unicode_literals

from .live_logger import LiveLogger


def create_instance(c_instance):
    return LiveLogger(c_instance)
