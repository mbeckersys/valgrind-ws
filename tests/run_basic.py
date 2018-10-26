#!/usr/bin/python
from lib import testbase

DESC = "Checking whether it runs..."


def check_result(fname):
    return True


result = testbase.run(DESC, __file__, ['sleep', '2'])
testbase.analyze(result, check_result)

