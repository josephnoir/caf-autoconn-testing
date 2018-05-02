# CAF autoconnect testing

I'm currently working to improve the way distributed CAF nodes are connected. Traditionally, CAF tracked routes via other nodes to achieve connectivity. Since the management of an overlay network is complicated and error prone, a new model exchanges contact information for nodes and dynamically builds connections between nodes when actors from new nodes are encountered.

## What's going on

My test setup has one master and eight nodes. The applications on all nodes will be started simultaneously via `sshcluster`.

Apps:
* Ping: build a ring, let each node forward an actor along the ring, collect pings from all other nodes.

## Dependencies

* Cmake
* C++ 11 compiler
* CAF (currently the branch topic/improve-autoconnect)

## Build

```
$ ./setup.sh    # if you don't have CAF installed
$ ./configure
$ make
```
