#!/usr/bin/python

import os
import sys
import argparse
import logging
import coloredlogs
import re
import numpy as np
import matplotlib.pyplot as plt


level = logging.INFO
if level == logging.DEBUG:
    fmt = " %(levelname)s %(module)s:%(funcName)s | %(message)s"
else:
    fmt = " %(levelname)s %(module)s | %(message)s"
coloredlogs.install(level=level, fmt=fmt)
log = logging.getLogger(__name__)


def plot_all(stats, info, args):
    # stats: [data]
    # data: {t=, wssd=, wssi=}

    ind = [d['t'] for d in stats]
    wssi = [d['wssi'] for d in stats]
    wssd = [d['wssd'] for d in stats]

    fig = plt.figure(figsize=(10, 5))
    ax = fig.add_subplot('111')
    mid = ind[0] + (ind[-1] - ind[0]) / 2

    ax.plot(ind, wssi, color='r', linestyle='-.')
    leg = ['insn']
    avgi = np.average(wssi)
    ax.plot([ind[0], mid, ind[-1]], [avgi] * 3, color='k', marker='x', linestyle='--')
    leg += ['insn-avg']

    ax.plot(ind, wssd, color='b')
    leg += ['data']
    avgd = np.average(wssd)
    ax.plot([ind[0], mid, ind[-1]], [avgd] * 3, color='k', marker='o', linestyle='--')
    leg += ['data-avg']

    if args.yscale is not None:
        ax.set_yscale(args.yscale)

    # axes and title
    ax.set_ylabel('working set size [pages]')
    tunit = info.get('Time Unit', '')
    ax.set_xlabel('time [{}]'.format(tunit) if tunit else 'time')
    tau = info.get('Tau', 0)
    cmd = info.get('Command', '')
    strtau = "with $\\tau$={:,}".format(tau) if tau != 0 else ''
    strcmd = "of '{}'".format(cmd) if cmd else ''
    title = "Working set size {} {}".format(strcmd, strtau) if args.title is None else args.title
    ax.set_title(title)

    if tau:
        ax.plot([0, tau], [0, 0], 'm', marker='v')
        leg += ['$\\tau$ length']

    # decoration
    ax.grid()
    ax.legend(leg)

    if args.outfile is not None:
        fig.savefig(args.outfile, bbox_inches='tight')
    else:
        plt.show()


def parse_file(fname):

    def human_number_to_int(st):
        try:
            num = int(st.replace(',', ''))
        except ValueError:
            num = None
        return num

    ret = []
    info = {}
    if not os.path.isfile(fname):
        log.error("File {} does not exist".format(fname))
        return None

    l = 0
    state = 'preamble'
    rex = re.compile(r"^[\s\t]*(\d+)[\s\t]+(\d+)[\s\t]+(\d+)[\s\t]+$")
    with open(fname, 'r') as f:
        for line in f:
            l += 1

            if state == "preamble":
                m = re.match(r"([^:]+):[\s\t]*(.*)$", line)
                if m:
                    k = m.group(1)
                    v = m.group(2)
                    if k in ('Tau', 'Every', 'Instructions', 'Page size'):
                        v = human_number_to_int(v.split(' ')[0])
                    info[k] = v
                    log.debug("Preamble: {}={}".format(k, v))
                if re.match(r'--', line):
                    state = 'search'

            if state == 'search' and re.match(r"^Working sets:", line):
                state = "wset"
                log.info("Found WS data in line {}".format(l))

            if state == "wset":
                m = rex.match(line)
                if m:
                    t = int(m.group(1))
                    wssi = int(m.group(2))
                    wssd = int(m.group(3))
                    ret.append(dict(t=t, wssi=wssi, wssd=wssd))
                    log.debug("wset point: t={}, i={}, d={}".format(t, wssi, wssd))
                if re.match(r'--', line):
                    state = 'search'
    # --
    log.info("Parsed {} lines, found {} data points".format(l, len(ret)))
    return ret, info


def process(args):
    """parse the file and plot"""
    stats, info = parse_file(args.file)
    if stats:
        log.info("Successfully parsed files: {}".format(args.file))
    else:
        log.error("No data found in file")
        return

    plot_all(stats, info, args=args)
    # --
    return 0


def main(argv):
    ##############
    # parse opts
    ##############
    parser = argparse.ArgumentParser(description='Plot output of valgrind-ws')

    parser.add_argument('-y', '--yscale', default='linear',
                        choices=['linear', 'symlog', 'log'],
                        help='choose scaling for y axis')
    parser.add_argument('-o', '--outfile', default=None,
                        help='filename to save plot')
    parser.add_argument('-t', '--title', default=None,
                        help='title of plot')

    # positional arguments
    parser.add_argument("file", help='file name to be parsed')

    args = parser.parse_args()
    return process(args)


if __name__ == "__main__":
    main(sys.argv[1:])

