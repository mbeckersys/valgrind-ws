#!/usr/bin/python

# stream implementation of algorithm from https://stackoverflow.com/a/22640362/6029703
import sys
import collections
import numpy as np
import pylab
import re


lag = 100  # moving window length
threshold = 10  # peak is detected if data point is that many variances away from mean
influence = .5  # how much peaks change thresholds. 0=none/robust, 1=fast/reactive
typefilt = 'exp'  # exp=exponential (lower memory footprint, less accurate), window=explicit
assert lag >= 1

alpha = float(2) / (lag + 1)


def peak_detect(y):
    """Successively consumes data points and finds peaks.
    A filter is used to smooth out peaks for robustness.
    """
    def update_avg(f):
        """moving average"""
        if pd_data['k'] == 0:
            avg = f
        else:
            if typefilt == 'window':
                avg = np.mean(np.array(pd_data['window']))
            else:
                pre = pd_data['movingAvg']
                avg = alpha * f + (1 - alpha) * pre
        pd_data['movingAvg'] = avg
        return avg

    def update_var(f):
        """moving variance"""
        if pd_data['k'] == 0:
            var = 0
        else:
            if typefilt == 'window':
                var = np.var(np.array(pd_data['window']))
            else:
                diff = f - pd_data['movingAvg']  # previous average!
                var = (1 - alpha) * (pd_data['movingVar'] + alpha * diff * diff)
        pd_data['movingVar'] = var
        return var

    is_peak = abs(y - pd_data['movingAvg']) > threshold * pd_data['movingVar']
    # 'movingVar'])
    if pd_data['k'] > 0 and is_peak:
        # peak: check direction and filter
        pk = 1 if y > pd_data['movingAvg'] else -1
        if pd_data['k'] >= lag:
            filtered = influence * y + (1 - influence) * pd_data['pre_filtered']  # exponential
        else:
            filtered = y
    else:
        pk = 0
        filtered = y

    # update peak detector data
    if typefilt == 'window': pd_data['window'].append(filtered)  # ring buffer for SMA
    update_var(filtered)  # must come before avg!!!
    update_avg(filtered)
    pd_data['pre_filtered'] = filtered
    pd_data['k'] += 1

    # only for plotting
    dbg_filt[dbg_idx] = filtered
    dbg_avg[dbg_idx] = pd_data['movingAvg']
    dbg_var[dbg_idx] = pd_data['movingVar']
    return pk


############
# test case
############

if len(sys.argv) > 1:
    datafile = sys.argv[1]
    stream = []
    with open(datafile, 'r') as f:
        for line in f:
            m = re.search(r"(\d+)[\s\t]+(\d+)[\s\t]+(\d+)", line)
            if m:
                stream.append(int(m.group(3)))  # 2=insn, 3=data
    stream = np.array(stream)
else:
    # data stream
    stream = np.array(
        [1, 1, 1.1, 1, 0.9, 1, 1, 1.1, 1, 0.9, 1, 1.1, 1, 1, 0.9, 1, 1, -1.1, 1, 1, 1, 1, 1.1, 0.9, 1,
         1.1, 1, 1, 0.9, 1, 1.1, -1, 1, 1.1, 1, 0.8, 0.9, 1, 1.2, 0.9, 1, 1, 1.1, 1.2, 1, 1.5, 1, 4, 2,
         5, 3, 2, 1, 1, 1, 0.9, 1, 1, 3, 2.6, 4, 3, 3.2, 2, 1, 1, 0.8, 4, 4, 2, 2.5, 1, 1, 1,
         1, 1, 1.1, 1, 0.9, 1, 1, 1.1, 1, 0.9, 1, 1.1, 1, 1, 0.9, 1, 1, -1.1, 1, 1, 1, 1, 1.1, 0.9, 1])

# internal data of peak detector
w = collections.deque([], maxlen=lag) if typefilt == 'window' else None  # ring buffer
pd_data = dict(pre_filtered=0,
               movingAvg=0,
               movingVar=0,
               window=w,
               k=0)

# this is just for visualization
dbg_filt = np.zeros(len(stream))
dbg_avg = np.zeros(len(stream))
dbg_var = np.zeros(len(stream))
signals = np.zeros(len(stream))

# streaming of data, one by one
pre = None
pk = 0
for i in range(0, len(stream)):
    dbg_idx = i
    signals[i] = peak_detect(stream[i])
    if i > 0 and signals[i] != pre:
        pre = signals[i]
        pk += 1
    pre = signals[i]
print "Peaks: {}".format(pk)

dbg_std = np.sqrt(dbg_var)

# Plot result
result = np.asarray(signals)
ax1 = pylab.subplot(211)
ax1.plot(np.arange(1, len(stream)+1), stream, color='k')
ax1.plot(np.arange(1, len(stream)+1), dbg_filt, color='red', lw=1)
ax1.plot(np.arange(1, len(stream)+1), dbg_avg, color="cyan", lw=2)
ax1.plot(np.arange(1, len(stream)+1), dbg_avg + threshold * dbg_std, color="green", lw=2)
ax1.plot(np.arange(1, len(stream)+1), dbg_avg - threshold * dbg_std, color="green", lw=2)
ax1.grid()
pylab.legend(['signal', 'filtered', 'avg', 'thresh', 'thresh'])
pylab.title("mode={}".format(typefilt))

pylab.subplot(212, sharex=ax1)
pylab.step(np.arange(1, len(stream)+1), result, color="red", lw=2)
pylab.ylim(-1.5, 1.5)
pylab.grid()
pylab.title("peak detector output")

pylab.show()
