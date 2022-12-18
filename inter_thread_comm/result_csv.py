import csv
import pprint
import numpy

mq_time_list = []
ev_time_list = []
cond_signal_time_list = []

with open('./sample.csv') as f:
    reader = csv.reader(f)
    for row in reader:
        if row[2] == 'mq':
            mq_time_list.append(float(row[1]) - float(row[0]))
        if row[2] == 'eventfd':
            ev_time_list.append(float(row[1]) - float(row[0]))
        if row[2] == 'cond_signal':
            cond_signal_time_list.append(float(row[1]) - float(row[0]))
print('{:.9f}'.format(numpy.average(mq_time_list)))
print('{:.9f}'.format(numpy.average(ev_time_list)))
print('{:.9f}'.format(numpy.average(cond_signal_time_list)))