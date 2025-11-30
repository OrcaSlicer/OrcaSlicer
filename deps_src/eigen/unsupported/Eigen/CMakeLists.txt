set(Eigen_HEADERS
  AdolcForward
  AlignedVector3
  ArpackSupport
  AutoDiff
  BVH
  EulerAngles
  FFT
  IterativeSolvers
  KroneckerProduct
  LevenbergMarquardt
  MatrixFunctions
  MPRealSupport
  NNLS
  NonLinearOptimization
  NumericalDiff
  OpenGLSupport
  Polynomials
  SparseExtra
  SpecialFunctions
  Splines
  )

install(FILES
  ${Eigen_HEADERS}
  DESTINATION ${INCLUDE_INSTALL_DIR}/unsupported/Eigen COMPONENT Devel
  )

install(DIRECTORY src DESTINATION ${INCLUDE_INSTALL_DIR}/unsupported/Eigen COMPONENT Devel FILES_MATCHING PATTERN "*.h")

add_subdirectory(CXX11)
