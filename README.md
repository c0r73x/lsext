[![Build Status](http://fredrik.fulhax.nu:8090/view/All/job/c0r73x/job/lsext/job/master/badge/icon)](http://fredrik.fulhax.nu:8090/view/All/job/c0r73x/job/lsext/job/master/)

# lsext
My personal extended version of ls with git support, colors and other fancy
stuff

Inspired by coreutils, ls++ and others.


## Customize
See example [config](https://github.com/c0r73x/lsext/blob/master/lsext.ini.sample) 

## Dependencies
------------------
| Name        |    Required        |   |
| ------------- |:-------------:| -----:|
| Iniparser     | **Required** | Recommended version 4.x, min 3.1 |
| re2 | **Required** | |
| OpenMP     | Optional | **Enabled by default** |
| libgit2     | Optional | Recommended version 0.24.1+ **Enabled by default** |


## Build dependencies
CMake    
C++11 compatible compiler 

## Usage

#### Flags
------------------
| Name        |            |
| ------------- |:-------------:|
| c     | |
| L | | |
| M     |  |
| a     | |
| r     | |
| f     |  |
| t     | Sort by modified date |
| S     | Sort by size |
| A     | Sort by alphabetical |
| l     | |
| n     | Toggle colors (on/off)  |
| N     | Load with default, ignore config |
| F     |  |
| h     | Help |

## Licence
MIT License
