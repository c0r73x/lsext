 [![Build Status](http://fredrik.fulhax.nu:8090/view/All/job/c0r73x/job/lsext/job/master/badge/icon)](http://fredrik.fulhax.nu:8090/view/All/job/c0r73x/job/lsext/job/master/)

# lsext
My personal extended version of ls with git support, colors and other fancy
stuff

Inspired by coreutils, ls++ and others.

*Tested on Linux and Mac*

## Customize
See example [config](https://github.com/c0r73x/lsext/blob/master/lsext.ini.sample) 

Feel free to share your screenshots and configs here, [Screenshots and config](https://github.com/c0r73x/lsext/issues/8)

## Dependencies
------------------
| Name        |    Required        |   |
| ------------- |:-------------:| -----:|
| Iniparser     | **Required** | Recommended version 4.x, min 3.1 |
| re2 | **Required** | |
| OpenMP     | Optional | **Enabled by default** |
| libgit2     | Optional | version 0.28.0+ **Enabled by default** |


## Build dependencies
CMake    
C++11 compatible compiler 

## Usage

#### Flags
------------------
| short | long |
| ------------- | ------------- |
| | --help | 
| -f | --dirs-first | 
| -c "option" | --forced-columns="option" | 
| -F "option" |  --format="option" | 
| -l | --list | 
| -C | --no-color | 
| -L | --resolve-links | 
| -M | --resolve-mounts | 
| -r | --reversed | 
| -a | --show-hidden | 
| -t | --sort-date | 
| -A | --sort-name | 
| -S | --sort-size | 
| -X | --sort-type | 
| -n | --numeric-uid-gid | 

## Known issues

* Slow the first time it lists directories with lots of git repos (this is a limitation in libgit2)
* OpenMP don't work on OSX (compile without USE_OPENMP)
* git submodules don't show as git directories
* git statuses for files and directories inside submodules don't show correctly

## Licence
MIT License
