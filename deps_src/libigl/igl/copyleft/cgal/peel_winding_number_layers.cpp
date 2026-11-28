#include "peel_winding_number_layers.h"

#include <cassert>

#include "propagate_winding_numbers.h"

template<
typename DerivedV,
typename DerivedF,
typename DerivedW >
IGL_INLINE size_t igl::copyleft::cgal::peel_winding_number_layers(
        const Eigen::MatrixBase<DerivedV > & V,
        const Eigen::MatrixBase<DerivedF > & F,
        Eigen::PlainObjectBase<DerivedW>& W) {
    const size_t num_faces = F.rows();
    Eigen::VectorXi labels(num_faces);
    labels.setZero();

    Eigen::MatrixXi winding_numbers;
    igl::copyleft::cgal::propagate_winding_numbers(V, F, labels, winding_numbers);
    assert(winding_numbers.rows() == num_faces);
    assert(winding_numbers.cols() == 2);

    int min_w = winding_numbers.minCoeff();
    int max_w = winding_numbers.maxCoeff();
    assert(max_w > min_w);

    W.resize(num_faces, 1);
    for (size_t i=0; i<num_faces; i++) {
        W(i, 0) = winding_numbers(i, 1);
    }
    return max_w - min_w;
}
