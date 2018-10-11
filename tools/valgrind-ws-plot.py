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


def plot_all(stats, args):
    # stats: [data]
    # data: {t=, wssd=, wssi=}

    ind = [d['t'] for d in stats]
    wssi = [d['wssi'] for d in stats]
    wssd = [d['wssd'] for d in stats]

    fig = plt.figure(figsize=(15, 10))
    ax = fig.add_subplot('111')
    mid = ind[0] + (ind[-1] - ind[0]) / 2

    ax.plot(ind, wssi, color='r')
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

    ax.set_ylabel('working set size (pages)')
    ax.set_xlabel('time')
    #ax.set_xticks(ind)
    #ax.set_xticklabels(tuple(cats), rotation=90)
    ax.grid()
    ax.legend(leg)
    title = "Working set size over time" if args.title is None else args.title
    ax.set_title(title)

    if args.outfile is not None:
        fig.savefig(args.outfile, bbox_inches='tight')
    else:
        plt.show()


def parse_file(fname):
    ret = []
    if not os.path.isfile(fname):
        log.error("File {} does not exist".format(fname))
        return None

    l = 0
    state = 'search'
    rex = re.compile(r"^[\s\t]*(\d+)[\s\t]+(\d+)[\s\t]+(\d+)[\s\t]+$")
    with open(fname, 'r') as f:
        for line in f:
            l += 1
            if state != "inside":
                if re.match(r"^Working sets:", line):
                    state = "inside"
                    log.info("Found WS data in line {}".format(l))
            elif state == "inside":
                m = rex.match(line)
                if m:
                    t = int(m.group(1))
                    wssi = int(m.group(2))
                    wssd = int(m.group(3))
                    ret.append(dict(t=t, wssi=wssi, wssd=wssd))
    # --
    log.info("Parsed {} lines, found {} data points".format(l, len(ret)))
    return ret


def process(args):
    """parse the file and plot"""
    stats = parse_file(args.file)
    if stats:
        log.info("Successfully parsed files: {}".format(args.file))
    else:
        log.error("No data found in file")
        return

    plot_all(stats, args=args)
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

