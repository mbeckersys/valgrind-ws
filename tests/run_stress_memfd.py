#!/usr/bin/python
from lib import testbase


def check_result(fname):
    return True  # TODO


result = testbase.run(__file__, ['stress-ng', '--memfd', '1', '-t' '5'])
testbase.analyze(result, check_result)
