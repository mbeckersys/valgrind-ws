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
    peaks = {d['t']: d['peak'] for d in stats if d['peak'] is not None}

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

    tau = info.get('Tau', 0)
    if tau:
        ax.plot([0, tau], [0, 0], 'm', marker='v')
        leg += ['$\\tau$ length']

    # annotate peak info
    peak_color = 'green'
    for t, pkid in peaks.iteritems():
        plt.axvline(x=t, color='green', linestyle='dotted')
        ax.annotate('{}'.format(pkid),
                    xy=(t, 0), xycoords='data',
                    bbox=dict(boxstyle="round", fc="0.8", color=peak_color),
                    color=peak_color, xytext=(0, -20), textcoords='offset points')

    # axes and title
    ax.set_ylabel('working set size [pages]')
    tunit = info.get('Time Unit', '')
    ax.set_xlabel('time [{}]'.format(tunit) if tunit else 'time')
    cmd = info.get('Command', '')
    strtau = "with $\\tau$={:,}".format(tau) if tau != 0 else ''
    strcmd = "of '{}'".format(cmd) if cmd else ''
    title = "Working set size {} {}".format(strcmd, strtau) if args.title is None else args.title
    ax.set_title(title)

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
    hdr = True
    state = 'preamble'
    rex_nopeak = re.compile(r"^[\s\t]*(\d+)[\s\t]+(\d+)[\s\t]+(\d+)$")
    rex_peak = re.compile(r"^[\s\t]*(\d+)[\s\t]+(\d+)[\s\t]+(\d+)[\s\t]+(.*)$")
    with open(fname, 'r') as f:
        for line in f:
            l += 1

            if '--' == line.strip():
                state = 'search'
                log.debug("section end")
                continue

            if state == 'search':
                if re.match(r"^Working sets:", line):
                    state = "wset"
                    hdr = True
                    log.info("Found WS data in line {}".format(l))
                    continue

                if re.match(r"Peak info", line):
                    state = "peakinfo"
                    log.info("Found peak info in line {}".format(l))
                    continue

            if state == "preamble":
                m = re.match(r"([^:]+):[\s\t]*(.*)$", line)
                if m:
                    k = m.group(1)
                    v = m.group(2)
                    if k in ('Tau', 'Every', 'Instructions', 'Page size', 'Peak window'):
                        v = human_number_to_int(v.split(' ')[0])
                    info[k] = v
                    log.debug("Preamble: {}={}".format(k, v))

            if state == "wset":
                if hdr:
                    hdr = False
                    wset_header = re.findall(r"\w+", line)
                    log.debug("working set header: {}".format(wset_header))
                else:
                    parts = re.findall(r"[^\s\t]+", line)
                    if len(parts) == len(wset_header):
                        t = int(parts[wset_header.index('t')])
                        wssi = int(parts[wset_header.index('WSS_insn')])
                        wssd = int(parts[wset_header.index('WSS_data')])
                        try:
                            peak = int(parts[wset_header.index('peak')])
                        except (IndexError, ValueError):
                            peak = None
                        ret.append(dict(t=t, wssi=wssi, wssd=wssd, peak=peak))
                        log.debug("wset point: t={}, i={}, d={}, pk={}".format(t, wssi, wssd, peak))

            if state == "peakinfo":
                m = re.match(r"\[\s+(\d+)\] refs=(\d+), loc=(.*)$", line)
                if m:
                    if 'peaks' not in info: info['peaks'] = {}
                    pkid = int(m.group(1))
                    refs = int(m.group(2))
                    loc = m.group(3)
                    info['peaks'][pkid] = dict(refs=refs, loc=loc)
                    log.info("Peak [{}]: refs={}, loc={}".format(pkid, refs, loc))

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

