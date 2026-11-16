if (IN_GIT_REPO)
    set(CGAL_DIRECTORY_FLAG --directory ${BINARY_DIR_REL}/dep_CGAL-prefix/src/dep_CGAL)
endif ()

orcaslicer_add_cmake_project(
    CGAL
    # GIT_REPOSITORY https://github.com/CGAL/cgal.git
    # GIT_TAG        28811b671a12b5caa9e3688569dadbc6b3728fe6 # v6.2.1
    # For whatever reason, this keeps downloading forever (repeats downloads if finished)
    URL      https://github.com/CGAL/cgal/releases/download/v6.2.1/CGAL-6.2.1.zip
    URL_HASH SHA256=eebd737d9b7f0199647ba5c1f9f6c7a3d0651aacef9c2fd4dedc52da6c25edbe
    DEPENDS dep_Boost dep_Eigen dep_GMP dep_MPFR
)

include(GNUInstallDirs)
