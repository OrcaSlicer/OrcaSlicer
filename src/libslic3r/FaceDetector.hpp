#ifndef slic3r_FaceDetector_hpp_
#define slic3r_FaceDetector_hpp_

#include "Point.hpp"

namespace Slic3r {
class ModelObject;
class TriangleMesh;

class FaceDetector {
public:
	FaceDetector(std::vector<TriangleMesh>& tms, std::vector<Transform3d>& transfos, double sample_interval)
		: m_meshes(tms), m_transfos(transfos), m_sample_interval(sample_interval) {}

	void detect_exterior_face();

private:
	std::vector<TriangleMesh>& m_meshes;
	std::vector<Transform3d>& m_transfos;
	double m_sample_interval;
};

}

#endif // #ifndef slic3r_FaceDetector_hpp_
