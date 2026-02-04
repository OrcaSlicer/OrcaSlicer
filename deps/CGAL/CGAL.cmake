if (IN_GIT_REPO)
    set(CGAL_DIRECTORY_FLAG --directory ${BINARY_DIR_REL}/dep_CGAL-prefix/src/dep_CGAL)
endif ()

orcaslicer_add_cmake_project(
    CGAL
    # GIT_REPOSITORY https://github.com/CGAL/cgal.git
    # GIT_TAG        08b27d3db14039d926c3a89d12955aa28fd55171 # releases/CGAL-6.1.1
    # For whatever reason, this keeps downloading forever (repeats downloads if finished)
    URL      https://github.com/CGAL/cgal/releases/download/v6.1.1/CGAL-6.1.1.zip
    URL_HASH SHA256=a47e83555966db53070bc37e2d313ce4de1b78a5ee3d8693369a3e268bcd2b79
    DEPENDS dep_Boost dep_GMP dep_MPFR
)

include(GNUInstallDirs)
