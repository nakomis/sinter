cmake_minimum_required(VERSION 3.16)
# One copy of the Forth compiler, shared with its host build.
set(EXTRA_COMPONENT_DIRS ../../forth)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sinter)
