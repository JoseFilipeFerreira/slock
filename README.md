# :lock: slock

Simple screen locker utility for X with pixelized screenshot as background 
(Fork of the suckless [slock](https://tools.suckless.org/slock/)).

Now with :rocket:**blazing fast**:rocket: pixelization!

|            	            | time (ms) 	| speedup 	|
|------------	            |-----------	|---------	|
| original   	            |      23.6 	|    -    	|
| improved memory access 	|       6.8 	|    3.47 	|

> HPC is my hammer and now everything looks like a nail - me

## Requirements
In order to build slock you need the Xlib header files.

## Installation
Edit config.mk to match your local setup (installs into
the /usr/local namespace by default).

```bash
make clean install
```

## Running
Simply invoke the `slock` command. To get out of it, enter your password.
