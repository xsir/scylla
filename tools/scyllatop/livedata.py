import logging
import parseexception
import fnmatch
import time
import metric
import defaults


class LiveData(object):
    def __init__(self, metricPatterns, interval, collectd):
        logging.info('will query collectd every {0} seconds'.format(interval))
        self._startedAt = time.time()
        self._measurements = []
        self._interval = interval
        self._collectd = collectd
        self._initializeMetrics(metricPatterns)
        self._views = []
        self._stop = False

    def addView(self, view):
        self._views.append(view)

    @property
    def measurements(self):
        return self._measurements

    def _initializeMetrics(self, metricPatterns):
        if len(metricPatterns) > 0:
            self._setupUserSpecifiedMetrics(metricPatterns)
        else:
            self._setupUserSpecifiedMetrics(defaults.DEFAULT_METRIC_PATTERNS)

    def _setupUserSpecifiedMetrics(self, metricPatterns):
        availableSymbols = [m.symbol for m in metric.Metric.discover(self._collectd)]
        symbols = [symbol for symbol in availableSymbols if self._matches(symbol, metricPatterns)]
        for symbol in symbols:
                logging.info('adding {0}'.format(symbol))
                self._measurements.append(metric.Metric(symbol, self._collectd))

    def _matches(self, symbol, metricPatterns):
        for pattern in metricPatterns:
            match = fnmatch.fnmatch(symbol, pattern)
            if match:
                return True
        return False

    def go(self):
        while not self._stop:
            for metric in self._measurements:
                self._update(metric)

            for view in self._views:
                view.update(self)
            time.sleep(self._interval)

    def _update(self, metric):
        try:
            metric.update()
        except parseexception.ParseException:
            logging.exception('exception while updating metric {0}'.format(metric))

    def stop(self):
        self._stop = True
