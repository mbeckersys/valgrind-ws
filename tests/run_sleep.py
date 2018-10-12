#!/usr/bin/python
from lib import testbase


def check_result(fname):
    return True  # TODO


result = testbase.run(__file__, ['sleep', '2'])
testbase.analyze(result, check_result)

