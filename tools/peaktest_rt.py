#!/usr/bin/python

# stream  implementation of algorithm from https://stackoverflow.com/a/22640362/6029703
import collections
import numpy as np
import pylab


lag = 30  # moving window length
threshold = 5  # peak is detected if data point is that many stddevs away from mean
influence = .0  # 0=threshold recalculated w/o signal (robust), 1=only on signal (reactive)
assert lag >= 1


def peak_detect(y):
    """successively consumes data points and determines peaks

    filter smoothes out peaks for robustness
    """
    idx = pd_data['idx']
    y0 = y - pd_data['movingAvg']
    if abs(y0) > threshold * pd_data['movingStd']:
        # peak, filter
        if y > pd_data['movingAvg']:
            pk = 1
        else:
            pk = -1
        pd_data['window'].append(influence * y + (1 - influence) * pd_data['window'][-1])
    else:
        # no peak, no filter
        pk = 0
        pd_data['window'].append(y)
    dbg_filt[idx] = pd_data['window'][-1]
    # compute vma
    windata = np.array(pd_data['window'])
    pd_data['movingAvg'] = np.mean(windata)
    pd_data['movingStd'] = np.std(windata)
    pd_data['idx'] += 1
    return pk


# data stream
stream = np.array(
    [1, 1, 1.1, 1, 0.9, 1, 1, 1.1, 1, 0.9, 1, 1.1, 1, 1, 0.9, 1, 1, -1.1, 1, 1, 1, 1, 1.1, 0.9, 1,
     1.1, 1, 1, 0.9, 1, 1.1, -1, 1, 1.1, 1, 0.8, 0.9, 1, 1.2, 0.9, 1, 1, 1.1, 1.2, 1, 1.5, 1, 3, 2,
     5, 3, 2, 1, 1, 1, 0.9, 1, 1, 3, 2.6, 4, 3, 3.2, 2, 1, 1, 0.8, 4, 4, 2, 2.5, 1, 1, 1])

# internal data of peak detector
pd_data = dict(movingAvg=0,
               movingStd=0,
               window=collections.deque(stream[0:lag], maxlen=lag),  # ring buffer
               idx=lag)

# this is just for visualization
dbg_filt = np.zeros(len(stream))
signals = np.zeros(len(stream))

# initialization
pd_data['movingAvg'] = np.mean(stream[0:lag])
pd_data['movingStd'] = np.std(stream[0:lag])

# streaming of data, one by one. FIXME: let detector bootstrap itself
for i in range(lag, len(stream)):
    signals[i] = peak_detect(stream[i])

# Plot result
result = np.asarray(signals)
pylab.subplot(211)
pylab.plot(np.arange(1, len(stream)+1), stream)
pylab.plot(np.arange(1, len(stream)+1), dbg_filt, color='red', lw=1)

pylab.subplot(212)
pylab.step(np.arange(1, len(stream)+1), result, color="red", lw=2)
pylab.ylim(-1.5, 1.5)

pylab.show()
