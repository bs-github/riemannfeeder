riemannfeeder
==============

Naemon event broker module (NEB) to feed check execution results as events into Riemann.

Known to work on Linux (eg Ubuntu 14.04 LTS) with:
- Naemon 1.0.5

## versioning

Versioning is done by tagging.
Add a tag into git; that tag will become the version number during compile time via a define in the Makefile.

## compiling

First clone the git repo to get the source.
``` bash
$ git clone https://github.com/bs-github/riemannfeeder.git
$ cd riemannfeeder
```
Install the following packages as root (especially naemon-dev from the consol labs repo https://labs.consol.de/repo/stable/):
``` bash
$ sudo aptitude install build-essential gcc bison autoconf automake flex libltdl-dev pkg-config libgtk2.0-dev naemon-dev
```
Then run make to compile.
``` bash
$ make
```
This will fetch and compile the dependencies for you as well.

If you build following the steps in this README, you get something like this:
```
$ ls -l *.o
lrwxr-xr-x 1 vagrant vagrant    44 Jul 20 05:35 riemannfeeder-latest.o -> riemannfeeder-v0.0.6-beta-6-ga1a41d8-dirty.o
-rwxr-xr-x 1 vagrant vagrant 81416 Jul 20 05:35 riemannfeeder-v0.0.6-beta-6-ga1a41d8-dirty.o
```
Linked is the latest version of the module built for Naemon.

## usage

Configure Naemon to load the neb module in *naemon.cfg* by adding the following line.
Alter the riemann host, and port according to your needs.
``` cfg
broker_module=/tmp/riemannfeeder.o riemann_host=localhost,riemann_port=5555,timeout=1
```

You can feed multiple target databases by specifying them on the module load line.
```
broker_module=/tmp/riemannfeeder.o riemann_host=localhost,riemann_port=5555,riemann_host=remotehost,riemann_port=5555
```

## options

Besides the **riemann_host**, **riemann_port** options, which can be given multiple times to provide data to more than one riemann instance, there are more options that you can set.

- **timeout** in seconds (defaults to 1,5 seconds, but you can only specify an integer here)

  *Warning: this will block nagios for the time it is waiting on riemann not only once but every $riemann_connect_retry_interval seconds!*

- **riemann_connect_retry_interval** in seconds (defaults to 15 seconds)
