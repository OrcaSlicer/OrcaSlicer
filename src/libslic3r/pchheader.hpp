#ifdef WIN32
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
#endif

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include <cassert>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <algorithm>
#include <atomic>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <istream>
#include <limits>
#include <list>
#include <locale>
#include <map>
#include <memory>
#include <numeric>
#include <ostream>
#include <queue>
#include <regex>
#include <set>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/erase.hpp>
#include <boost/algorithm/string/find.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/bind/bind.hpp>
#include <boost/config.hpp>
#include <boost/config/warning_disable.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/date_time/local_time/local_time.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/foreach.hpp>
#include <boost/format.hpp>
#include <boost/geometry.hpp>
#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/geometries/point.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/index/rtree.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/locale.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/trivial.hpp>
#include <boost/multi_array.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/filesystem.hpp>
#include <boost/nowide/iostream.hpp>
#include <boost/regex.hpp>

// boost/property_tree/json_parser/detail/parser.hpp includes boost/bind.hpp, which is deprecated.
// Suppress the following boost message:
// The practice of declaring the Bind placeholders (_1, _2, ...) in the global namespace is deprecated.
#define BOOST_BIND_GLOBAL_PLACEHOLDERS
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#undef BOOST_BIND_GLOBAL_PLACEHOLDERS

#include <boost/thread.hpp>
#include <boost/version.hpp>

#include <tbb/parallel_for.h>
#include <tbb/spin_mutex.h>
#include <tbb/task_group.h>

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <cereal/access.hpp>
#include <cereal/types/base_class.hpp>

#include <clipper/clipper_z.hpp>
#include "clipper.hpp"
#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "Config.hpp"
#include "enum_bitmask.hpp"
#include "format.hpp"
#include "I18N.hpp"
#include "MultiPoint.hpp"
#include "Point.hpp"
#include "Polygon.hpp"
#include "Polyline.hpp"
#include "SVG.hpp"

#include "libslic3r.h"
#include "libslic3r_version.h"

#include <Shiny/Shiny.h>

#include <admesh/stl.h>
