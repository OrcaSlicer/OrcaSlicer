#/|/ Copyright (c) Prusa Research 2021 - 2022 Tomáš Mészáros @tamasmeszaros, Filip Sykala @Jony01
#/|/
#/|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#/|/
orcaslicer_add_cmake_project(Cereal
    URL "https://github.com/USCiLab/cereal/archive/refs/tags/v1.3.0.zip"
    URL_HASH SHA256=71642cb54658e98c8f07a0f0d08bf9766f1c3771496936f6014169d3726d9657
    CMAKE_ARGS
        -DJUST_INSTALL_CEREAL=ON
        -DSKIP_PERFORMANCE_COMPARISON=ON
        -DBUILD_TESTS=OFF
)