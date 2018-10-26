#!/usr/bin/python
import os
import re
import math
from lib import testbase
from subprocess import call

DESC = "Checking WSS statistics..."

EXE = "pageramp/pageramp"
MAXPAGES = 1024
CYCLES = 10


def check_result(fname):
    if not os.path.isfile(fname):
        print "File {} not found".format(fname)
        return False

    ok_dtot = False
    ok_davg = False
    ok_dpk = False
    ok_dvar = False
    pk = None
    avg = None
    var = None
    npg = None
    with open(fname, 'r') as f:
        for line in f:
            m = re.search(r"Data WSS avg/var/peak:[\s\t]*([\d\.,]+)/([\d\.,]+)/([\d\.,]+).*", line)
            if m:
                avg = testbase.human_to_number(m.group(1))
                var = testbase.human_to_number(m.group(2))
                pk = testbase.human_to_number(m.group(3))
                ok_davg = 0.5*MAXPAGES <= avg <= 0.7*MAXPAGES
                ok_dpk  = MAXPAGES <= pk <= 1.1*MAXPAGES
                ok_dvar = 0.2*MAXPAGES <= math.sqrt(var) <= 0.3*MAXPAGES
            m = re.search(r"Data pages/access:[\s\t]*([\d\.,]+) pages.*", line)
            if m:
                npg = testbase.human_to_number(m.group(1))
                ok_dtot = MAXPAGES <= npg <= 1.1*MAXPAGES
    ok_rel = npg >= avg
    if ok_dtot and ok_davg and ok_dpk and ok_rel and ok_dvar:
        return True
    else:
        print "Unexpected: ok_tot={}, ok_avg={}, ok_peak={}, ok_var={}, ok_rel={}".format\
            (ok_dtot, ok_davg, ok_dpk, ok_dvar, ok_rel)
        return False


if not os.path.isfile(EXE):
    opwd = os.getcwd()
    os.chdir(os.path.dirname(EXE))
    call(['make'])
    os.chdir(opwd)

if not os.path.isfile(EXE):
    print "{}: Failed to locate executable".format(__file__)
    exit(1)

result = testbase.run(DESC, __file__, [EXE, str(MAXPAGES), str(CYCLES)])
testbase.analyze(result, check_result)
