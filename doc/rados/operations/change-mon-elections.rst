.. _changing_monitor_elections:

==================================
Configure Monitor Election Strategies
==================================

By default, the monitors will use the CLASSIC option it has always used. We
recommnd you stay in this mode unless you require features in the other
modes.

If you want to switch modes BEFORE constructing the cluster, change
the ``mon election default strategy`` option. This option is an integer value:

* 1 for CLASSIC
* 2 for DISALLOW
* 3 for CONNECTIVITY

Once your cluster is running, you can change strategies by running ::

  $ ceph mon set election strategy {CLASSIC|DISALLOW|CONNECTIVITY}

Choosing a mode
===============
The modes other than CLASSIC provide different features. We recommend
you stay in CLASSIC mode if you don't need the extra features as it is
the simplest mode.

The DISALLOW Mode
=================
This mode lets you name monitors as DISALLOWED, in which case they will
participate in the quorum and serve clients, but cannot be elected leader. You
may wish to use this if you have some monitors which are known to be far away
from clients 
You can disallow a leader by running ::

  $ ceph mon add disallowed leader {name}

You can remove a monitor from the disallowed list, and allow it to become
a leader again, by running ::

  $ ceph mon rm disallowed leader {name}

The CONNECTIVITY Mode
=====================
This mode evaluates connection scores provided by each monitor for its
peers and elects the monitor with the best score. This mode is designed
to handle netsplits, which may happen if your cluster is stretched across
multiple data centers or otherwise susceptible.

This mode also supports disallowing monitors from being the leader
using the same commands as above in DISALLOW.

Examining connectivity scores
=============================
The monitors maintan connection scores even if they aren't in
the CONNECTIVITY election mode. You can examine the scores a monitor
has by running ::

  ceph daemon mon.{name} connection scores dump

If for some reason you experience problems and troubleshooting makes
you think your scores have become invalid, you can forget history and
reset them by running ::

  ceph daemon mon.{name} connection scores reset
