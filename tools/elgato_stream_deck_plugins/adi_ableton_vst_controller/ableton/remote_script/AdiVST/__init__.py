# -*- coding: utf-8 -*-
"""AdiVST Remote Script package entry point.

Live calls create_instance(c_instance) once when the control surface is selected.
"""
from __future__ import absolute_import

from .AdiVST import AdiVST


def create_instance(c_instance):
    return AdiVST(c_instance)
