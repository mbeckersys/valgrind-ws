import os
import re
import subprocess


def outfile(testname):
    return os.path.splitext(os.path.basename(testname))[0] + '.%p.log'


def run(caller, args):
    blob = subprocess.check_output(['valgrind', '--tool=ws', '--ws-file={}'.format
                                   (outfile(caller))] + args, stderr=subprocess.STDOUT)
    return blob.split("\n")


def human_to_number(st):
    try:
        num = float(st.replace(',', ''))
    except ValueError:
        num = None
    return num


def get_outfile(stdout):
    fname = None
    for line in stdout:
        m = re.search(r"Writing results to file '(.*)'", line)
        if m:
            fname = m.group(1)
            break
    return fname


def analyze(stdout, func_check):
    """
    Call test-specific check and exit with common formatting.

    func_check: function "pointer" for checks to be executed.
    """
    fname = get_outfile(stdout)
    if func_check(fname):
        os.remove(fname)
        print "PASSED"
        exit(0)
    else:
        print "FAILED. See {}".format(fname)
        exit(1)

