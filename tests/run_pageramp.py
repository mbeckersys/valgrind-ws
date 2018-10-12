#!/usr/bin/python
import os
import re
from lib import testbase
from subprocess import call


EXE = "pageramp/pageramp"
MAXPAGES = 1024
CYCLES = 10


def check_result(fname):
    if not os.path.isfile(fname):
        print "File {} not found".format(fname)
        return False

    ok_dtot = False
    ok_davg = False
    with open(fname, 'r') as f:
        for line in f:
            m = re.search(r"Data avg/peak/total:[\s\t]*([\d\.,]+)/([\d\.,]+)/([\d\.,]+).*", line)
            if m:
                a = testbase.human_to_number(m.group(1))
                p = testbase.human_to_number(m.group(2))
                t = testbase.human_to_number(m.group(3))
                ok_dtot = MAXPAGES <= t <= 1.1*MAXPAGES
                ok_davg = 0.5*MAXPAGES <= a <= 0.7*MAXPAGES
    if ok_dtot and ok_davg:
        return True
    else:
        print "Unexpected: ok_dtot={}, ok_davg={}".format(ok_dtot, ok_davg)
        return False


if not os.path.isfile(EXE):
    opwd = os.getcwd()
    os.chdir(os.path.dirname(EXE))
    call(['make'])
    os.chdir(opwd)

if not os.path.isfile(EXE):
    print "{}: Failed to locate executable".format(__file__)
    exit(1)

result = testbase.run(__file__, [EXE, str(MAXPAGES), str(CYCLES)])
testbase.analyze(result, check_result)
