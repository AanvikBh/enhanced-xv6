## xv6 Report: Priority Based Scheduling (PBS)

# Implementation:

- Declared the following variables in kernel/proc.h struct proc:
  uint wtime; -> Update if state is RUNNABLE
  uint Rtime; -> Update if state is RUNNING
  uint stime; -> Update if state is SLEEPING
  int staticPriority;
  int RBI;
  int dynamicPriority;
  int numScheduled;

- In allocproc(), staticPriority is set as 50, RBI value is set as 25, and dynamicPriority=min(SP+RBI, 100)=min(50+25, 100)=75.

- Since this PBS is preemptive, i.e. CPU takes control by interrupts using yield(), all of the time metrics(rtime, stime, wtime) are updated in the clockintr() function by looking at p->state value. 

- In kernel/proc.c, void calculatePriorities(struct proc *p) is defined which will update the values of the remaining metrics defined to decide the priority of the process, using the formula mentioned in the doc. We reset all the time metrics of the process that we have decided to schedule.

- If set_priority() is called, then it first updates the value of the static priority, then checks it with the old static priority, and then calls yield() to reschedule if new < old. It will also set RBI value to 25, and reset all the time metrics. 

# Assumption

A process who has been scheduled lesser number of times will have higher priority, and the process which is created more recently, i.e. has higher ctime value, will have higher priority. 


# Analysis

For CPUS=1
It is behaving like FCFS, with I/O bound processes being processed at the end. We also note that on running schedulertest multiple times, we always get the same output. We can say that scheduling is being done based on ctime, which implies that RBI value for CPUS=1 does not change that much for all the non-I/O bound processes.

For CPUS>=2
It is selecting random processes, and there is no particular order observed. This output varies everytime schedulertest is run. On increasing CPUS, avg rtime and wtime decreases. Here, RBI will also vary for non I/O bound processes. 

