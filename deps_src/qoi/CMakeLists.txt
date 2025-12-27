# PrusaSlicer specific CMake

cmake_minimum_required(VERSION 3.13)
project(qoi)

add_library(qoi STATIC
    qoi.h
    qoilib.c
)
