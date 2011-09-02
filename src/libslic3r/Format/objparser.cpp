#include <stdlib.h>
#include <string.h>

#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdio.hpp>

#include "objparser.hpp"

#include "libslic3r/LocalesUtils.hpp"

namespace ObjParser {
#define EATWS()  while (*line == ' ' || *line == '\t') ++line
static bool obj_parseline(const char *line, ObjData &data)
{
	if (*line == 0)
		return true;
    assert(Slic3r::is_decimal_separator_point());
	// Ignore whitespaces at the beginning of the line.
	//FIXME is this a good idea?
	EATWS();

	char c1 = *line ++;
	switch (c1) {
	case '#':
		// Comment, ignore the rest of the line.
		break;
	case 'v':
	{
		// Parse vertex geometry (position, normal, texture coordinates)
		char c2 = *line ++;
		switch (c2) {
		case 't':
		{
			// vt - vertex texture parameter
			// u v [w], w == 0 (or w == 1)
			char c2 = *line ++;
			if (c2 != ' ' && c2 != '\t')
				return false;
			EATWS();
			char *endptr = 0;
			double u = strtod(line, &endptr);
			if (endptr == 0 || (*endptr != ' ' && *endptr != '\t'))
				return false;
			line = endptr;
			EATWS();
			double v = 0;
			if (*line != 0) {
				v = strtod(line, &endptr);
				if (endptr == 0 || (*endptr != ' ' && *endptr != '\t' && *endptr != 0))
					return false;
				line = endptr;
				EATWS();
			}
			/*double w = 0;
			if (*line != 0) {
				w = strtod(line, &endptr);
				if (endptr == 0 || (*endptr != ' ' && *endptr != '\t' && *endptr != 0))
					return false;
				line = endptr;
				EATWS();
			}*/
			if (*line != 0)
				return false;
			data.textureCoordinates.push_back((float)u);
			data.textureCoordinates.push_back((float)v);
			//data.textureCoordinates.push_back((float)w);
			break;
		}
		case 'n':
		{
			// vn - vertex normal
			// x y z
			char c2 = *line ++;
			if (c2 != ' ' && c2 != '\t')
				return false;
			EATWS();
			char *endptr = 0;
			double x = strtod(line, &endptr);
			if (endptr == 0 || (*endptr != ' ' && *endptr != '\t'))
				return false;
			line = endptr;
			EATWS();
			double y = strtod(line, &endptr);
			if (endptr == 0 || (*endptr != ' ' && *endptr != '\t'))
				return false;
			line = endptr;
			EATWS();
			double z = strtod(line, &endptr);
			if (endptr == 0 || (*endptr != ' ' && *endptr != '\t' && *endptr != 0))
				return false;
			line = endptr;
			EATWS();
			if (*line != 0)
				return false;
			data.normals.push_back((float)x);
			data.normals.push_back((float)y);
			data.normals.push_back((float)z);
			break;
		}
		case 'p':
		{
			// vp - vertex parameter
			char c2 = *line ++;
			if (c2 != ' ' && c2 != '\t')
				return false;
			EATWS();
			char *endptr = 0;
			double u = strtod(line, &endptr);
			if (endptr == 0 || (*endptr != ' ' && *endptr != '\t' && *endptr != 0))
				return false;
			line = endptr;
			EATWS();
			double v = strtod(line, &endptr);
			if (endptr == 0 || (*endptr != ' ' && *endptr != '\t' && *endptr != 0))
				return false;
			line = endptr;
			EATWS();
			double w = 0;
			if (*line != 0) {
				w = strtod(line, &endptr);
				if (endptr == 0 || (*endptr != ' ' && *endptr != '\t' && *endptr != 0))
					return false;
				line = endptr;
				EATWS();
			}
			if (*line != 0)
				return false;
			data.parameters.push_back((float)u);
			data.parameters.push_back((float)v);
			data.parameters.push_back((float)w);
			break;
		}
		default:
		{
			// v - vertex geometry
			if (c2 != ' ' && c2 != '\t')
				return false;
			EATWS();
			char *endptr = 0;
			double x = strtod(line, &endptr);
			if (endptr == 0 || (*endptr != ' ' && *endptr != '\t'))
				return false;
			line = endptr;
			EATWS();
			double y = strtod(line, &endptr);
			if (endptr == 0 || (*endptr != ' ' && *endptr != '\t'))
				return false;
			line = endptr;
			EATWS();
			double z = strtod(line, &endptr);
			if (endptr == 0 || (*endptr != ' ' && *endptr != '\t' && *endptr != 0))
				return false;
			line = endptr;
			EATWS();
            double color_x = 0.0, color_y = 0.0, color_z = 0.0, color_w = 0.0;//undefine color
            if (*line != 0) {
                if (!data.has_vertex_color) {
                    data.has_vertex_color = true;
                }
                color_x = strtod(line, &endptr);
                if (endptr == 0 || (*endptr != ' ' && *endptr != '\t' && *endptr != 0))
                    return false;
                line = endptr;
                EATWS();
                color_y = strtod(line, &endptr);
                if (endptr == 0 || (*endptr != ' ' && *endptr != '\t' && *endptr != 0))
                     return false;
                line = endptr;
                EATWS();
                color_z = strtod(line, &endptr);
                if (endptr == 0 || (*endptr != ' ' && *endptr != '\t' && *endptr != 0))
                    return false;
                line = endptr;
                EATWS();
                color_w = 1.0;//default define alpha = 1.0
                if (*line != 0) {
                    color_w = strtod(line, &endptr);
                    if (endptr == 0 || (*endptr != ' ' && *endptr != '\t' && *endptr != 0)) return false;
                    line = endptr;
                    EATWS();
                }
			}
            // the following check is commented out because there may be obj files containing extra data, as those generated by Meshlab,
            // see https://dev.prusa3d.com/browse/SPE-1019 for an example,
            // and this would lead to a crash because no vertex would be stored
//            if (*line != 0)
//                return false;
            data.coordinates.push_back((float)x);
			data.coordinates.push_back((float)y);
			data.coordinates.push_back((float)z);
            data.coordinates.push_back((float) color_x);
            data.coordinates.push_back((float) color_y);
            data.coordinates.push_back((float) color_z);
            data.coordinates.push_back((float) color_w);
			break;
		}
		}
		break;
	}
	case 'f':
	{
		// face
		EATWS();
		if (*line == 0)
			return false;

		// current vertex to be parsed
		ObjVertex vertex;
		char *endptr = 0;
		while (*line != 0) {
			// Parse a single vertex reference.
			vertex.coordIdx			= 0;
			vertex.normalIdx		= 0;
			vertex.textureCoordIdx	= 0;
			vertex.coordIdx = strtol(line, &endptr, 10);
			// Coordinate has to be defined
			if (endptr == 0 || (*endptr != ' ' && *endptr != '\t' && *endptr != '/' && *endptr != 0))
				return false;
			line = endptr;
			if (*line == '/') {
				++ line;
				// Texture coordinate index may be missing after a 1st slash, but then the normal index has to be present.
				if (*line != '/') {
					// Parse the texture coordinate index.
					vertex.textureCoordIdx = strtol(line, &endptr, 10);
					if (endptr == 0 || (*endptr != ' ' && *endptr != '\t' && *endptr != '/' && *endptr != 0))
						return false;
					line = endptr;
				}
				if (*line == '/') {
					// Parse normal index.
					++ line;
					vertex.normalIdx = strtol(line, &endptr, 10);
					if (endptr == 0 || (*endptr != ' ' && *endptr != '\t' && *endptr != 0))
						return false;
					line = endptr;
				}
			}
			if (vertex.coordIdx < 0)
                vertex.coordIdx += (int) data.coordinates.size() / OBJ_VERTEX_LENGTH;
            else
				-- vertex.coordIdx;
			if (vertex.normalIdx < 0)
                vertex.normalIdx += (int)data.normals.size() / 3;
            else
				-- vertex.normalIdx;
			if (vertex.textureCoordIdx < 0)
                vertex.textureCoordIdx += (int)data.textureCoordinates.size() / 3;
            else
				-- vertex.textureCoordIdx;
			data.vertices.push_back(vertex);
			EATWS();
		}
        if (data.usemtls.size() > 0) {
			data.usemtls.back().vertexIdxEnd = (int) data.vertices.size();
		}
        if (data.usemtls.size() > 0) {
            int face_index_count = 0;
            for (int i = data.vertices.size() - 1; i >= 0; i--) {
                if (data.vertices[i].coordIdx == -1) {
					break;
				}
                face_index_count++;
            }
            if (face_index_count == 3) {//tri
                data.usemtls.back().face_end++;
			} else if (face_index_count == 4) {//quad
                data.usemtls.back().face_end++;
                data.usemtls.back().face_end++;
			}
        }
		vertex.coordIdx			= -1;
		vertex.normalIdx		= -1;
		vertex.textureCoordIdx	= -1;
		data.vertices.push_back(vertex);
		break;
	}
	case 'm':
	{
		if (*(line ++) != 't' ||
			*(line ++) != 'l' ||
			*(line ++) != 'l' ||
			*(line ++) != 'i' ||
			*(line ++) != 'b')
			return false;
		// mtllib [external .mtl file name]
		// printf("mtllib %s\r\n", line);
		EATWS();
		data.mtllibs.push_back(std::string(line));
		break;
	}
	case 'u':
	{
		if (*(line ++) != 's' ||
			*(line ++) != 'e' ||
			*(line ++) != 'm' ||
			*(line ++) != 't' ||
			*(line ++) != 'l')
			return false;
		// usemtl [material name]
		// printf("usemtl %s\r\n", line);
		EATWS();
        if (data.usemtls.size()>0) {
			data.usemtls.back().vertexIdxEnd = (int) data.vertices.size();
		}
		ObjUseMtl usemtl;
        usemtl.vertexIdxFirst = (int)data.vertices.size();
        usemtl.name = line;
		data.usemtls.push_back(usemtl);
        if (data.usemtls.size() == 1) {
            data.usemtls.back().face_start = 0;
		}
		else {//>=2
            auto count       = data.usemtls.size();
            auto& last_usemtl = data.usemtls[count-1];
            auto& last_last_usemtl  = data.usemtls[count - 2];
            last_usemtl.face_start = last_last_usemtl.face_end + 1;
		}
        data.usemtls.back().face_end = data.usemtls.back().face_start - 1;
		break;
	}
	case 'o':
	{
		// o [object name]
		EATWS();
		while (*line != ' ' && *line != '\t' && *line != 0)
			++ line;
		// copy name to line.
		EATWS();
		if (*line != 0)
			return false;
		ObjObject object;
        object.vertexIdxFirst = (int)data.vertices.size();
        object.name = line;
		data.objects.push_back(object);
		break;
	}
	case 'g':
	{
		// g [group name]
		// printf("group %s\r\n", line);
		ObjGroup group;
        group.vertexIdxFirst = (int)data.vertices.size();
        group.name = line;
		data.groups.push_back(group);
		break;
	}
	case 's':
	{
		// s 1 / off
		char c2 = *line ++;
		if (c2 != ' ' && c2 != '\t')
			return false;
		EATWS();
		char *endptr = 0;
		long g = strtol(line, &endptr, 10);
		if (endptr == 0 || (*endptr != ' ' && *endptr != '\t' && *endptr != 0))
			return false;
		line = endptr;
		EATWS();
		if (*line != 0)
			return false;
		ObjSmoothingGroup group;
        group.vertexIdxFirst = (int)data.vertices.size();
        group.smoothingGroupID = g;
		data.smoothingGroups.push_back(group);
		break;
	}
	default:
    	BOOST_LOG_TRIVIAL(error) << "ObjParser: Unknown command: " << c1;
		break;
	}

	return true;
}
static std::string cur_mtl_name = "";
static bool        mtl_parseline(const char *line, MtlData &data)
{
    if (*line == 0) return true;
    assert(Slic3r::is_decimal_separator_point());
    // Ignore whitespaces at the beginning of the line.
    // FIXME is this a good idea?
    EATWS();

    char c1 = *line++;
    switch (c1) {
        case '#': {// Comment, ignore the rest of the line.
            break;
        }
		case 'n': {
			if (*(line++) != 'e' || *(line++) != 'w' || *(line++) != 'm' || *(line++) != 't' || *(line++) != 'l')
				return false;
			EATWS();
			ObjNewMtl new_mtl;
            cur_mtl_name = line;
            data.new_mtl_unmap[cur_mtl_name] = std::make_shared<ObjNewMtl>();
			break;
		}
        case 'm': {
            if (*(line++) != 'a' || *(line++) != 'p' || *(line++) != '_' || *(line++) != 'K' || *(line++) != 'd') return false;
            EATWS();
            if (data.new_mtl_unmap.find(cur_mtl_name) != data.new_mtl_unmap.end()) {
				data.new_mtl_unmap[cur_mtl_name]->map_Kd = line;
			}
            break;
        }
        case 'N': {
            char cur_char = *(line++);
            if (cur_char == 's') {
                EATWS();
                char * endptr = 0;
                double ns     = strtod(line, &endptr);
                if (data.new_mtl_unmap.find(cur_mtl_name) != data.new_mtl_unmap.end()) {
					data.new_mtl_unmap[cur_mtl_name]->Ns = (float) ns;
				}
            } else if (cur_char == 'i') {
                EATWS();
                char * endptr = 0;
                double ni    = strtod(line, &endptr);
                if (data.new_mtl_unmap.find(cur_mtl_name) != data.new_mtl_unmap.end()) {
					data.new_mtl_unmap[cur_mtl_name]->Ni = (float) ni;
				}
            }
            break;
        }
        case 'K': {
            char cur_char = *(line++);
            if (cur_char == 'a') {
                EATWS();
                char * endptr = 0;
                double x      = strtod(line, &endptr);
                if (endptr == 0 || (*endptr != ' ' && *endptr != '\t')) return false;
                line = endptr;
                EATWS();
                double y = strtod(line, &endptr);
                if (endptr == 0 || (*endptr != ' ' && *endptr != '\t')) return false;
                line = endptr;
                EATWS();
                double z = strtod(line, &endptr);
                if (endptr == 0 || (*endptr != ' ' && *endptr != '\t' && *endptr != 0)) return false;
                line = endptr;
                EATWS();
                if (data.new_mtl_unmap.find(cur_mtl_name) != data.new_mtl_unmap.end()) {
                    data.new_mtl_unmap[cur_mtl_name]->Ka[0] = x;
                    data.new_mtl_unmap[cur_mtl_name]->Ka[1] = y;
                    data.new_mtl_unmap[cur_mtl_name]->Ka[2] = z;
                }
            } else if (cur_char == 'd') {
                EATWS();
                char * endptr = 0;
                double x      = strtod(line, &endptr);
                if (endptr == 0 || (*endptr != ' ' && *endptr != '\t')) return false;
                line = endptr;
                EATWS();
                double y = strtod(line, &endptr);
                if (endptr == 0 || (*endptr != ' ' && *endptr != '\t')) return false;
                line = endptr;
                EATWS();
                double z = strtod(line, &endptr);
                if (endptr == 0 || (*endptr != ' ' && *endptr != '\t' && *endptr != 0)) return false;
                line = endptr;
                EATWS();
                if (data.new_mtl_unmap.find(cur_mtl_name) != data.new_mtl_unmap.end()) {
                    data.new_mtl_unmap[cur_mtl_name]->Kd[0] = x;
                    data.new_mtl_unmap[cur_mtl_name]->Kd[1] = y;
                    data.new_mtl_unmap[cur_mtl_name]->Kd[2] = z;
                }
            } else if (cur_char == 's') {
                EATWS();
                char * endptr = 0;
                double x      = strtod(line, &endptr);
                if (endptr == 0 || (*endptr != ' ' && *endptr != '\t')) return false;
                line = endptr;
                EATWS();
                double y = strtod(line, &endptr);
                if (endptr == 0 || (*endptr != ' ' && *endptr != '\t')) return false;
                line = endptr;
                EATWS();
                double z = strtod(line, &endptr);
                if (endptr == 0 || (*endptr != ' ' && *endptr != '\t' && *endptr != 0)) return false;
                line = endptr;
                EATWS();
                if (data.new_mtl_unmap.find(cur_mtl_name) != data.new_mtl_unmap.end()) {
                    data.new_mtl_unmap[cur_mtl_name]->Ks[0] = x;
                    data.new_mtl_unmap[cur_mtl_name]->Ks[1] = y;
                    data.new_mtl_unmap[cur_mtl_name]->Ks[2] = z;
                }
            } else if (cur_char == 'e') {
                EATWS();
                char * endptr = 0;
                double x      = strtod(line, &endptr);
                if (endptr == 0 || (*endptr != ' ' && *endptr != '\t')) return false;
                line = endptr;
                EATWS();
                double y = strtod(line, &endptr);
                if (endptr == 0 || (*endptr != ' ' && *endptr != '\t')) return false;
                line = endptr;
                EATWS();
                double z = strtod(line, &endptr);
                if (endptr == 0 || (*endptr != ' ' && *endptr != '\t' && *endptr != 0)) return false;
                line = endptr;
                EATWS();
                if (data.new_mtl_unmap.find(cur_mtl_name) != data.new_mtl_unmap.end()) {
                    data.new_mtl_unmap[cur_mtl_name]->Ke[0] = x;
                    data.new_mtl_unmap[cur_mtl_name]->Ke[1] = y;
                    data.new_mtl_unmap[cur_mtl_name]->Ke[2] = z;
                }
            }
            break;
        }
        case 'i': {
            if (*(line++) != 'l' || *(line++) != 'l' || *(line++) != 'u' || *(line++) != 'm')
				return false;
            EATWS();
            char * endptr = 0;
            double illum  = strtod(line, &endptr);
            if (data.new_mtl_unmap.find(cur_mtl_name) != data.new_mtl_unmap.end()) {
				data.new_mtl_unmap[cur_mtl_name]->illum = (float) illum;
			}
            break;
        }
        case 'd': {
            EATWS();
            char * endptr = 0;
            double d  = strtod(line, &endptr);
            if (data.new_mtl_unmap.find(cur_mtl_name) != data.new_mtl_unmap.end()) {
				data.new_mtl_unmap[cur_mtl_name]->d = (float) d;
			}
            break;
        }
        case 'T': {
            char cur_char = *(line++);
            if (cur_char == 'r') {
                EATWS();
                char * endptr = 0;
                double tr     = strtod(line, &endptr);
                if (data.new_mtl_unmap.find(cur_mtl_name) != data.new_mtl_unmap.end()) {
                    data.new_mtl_unmap[cur_mtl_name]->Tr = (float) tr;
                }
                break;
            } else if (cur_char == 'f') {
                EATWS();
                char * endptr = 0;
                double x      = strtod(line, &endptr);
                if (endptr == 0 || (*endptr != ' ' && *endptr != '\t')) return false;
                line = endptr;
                EATWS();
                double y = strtod(line, &endptr);
                if (endptr == 0 || (*endptr != ' ' && *endptr != '\t')) return false;
                line = endptr;
                EATWS();
                double z = strtod(line, &endptr);
                if (endptr == 0 || (*endptr != ' ' && *endptr != '\t' && *endptr != 0)) return false;
                line = endptr;
                EATWS();
                if (data.new_mtl_unmap.find(cur_mtl_name) != data.new_mtl_unmap.end()) {
                    data.new_mtl_unmap[cur_mtl_name]->Tf[0] = x;
                    data.new_mtl_unmap[cur_mtl_name]->Tf[1] = y;
                    data.new_mtl_unmap[cur_mtl_name]->Tf[2] = z;
                }
                break;
            }
        }
    }
    return true;
}

bool objparse(const char *path, ObjData &data)
{
    Slic3r::CNumericLocalesSetter locales_setter;

	FILE *pFile = boost::nowide::fopen(path, "rt");
	if (pFile == 0)
		return false;

	try {
		char buf[65536 * 2];
		size_t len = 0;
		size_t lenPrev = 0;
		while ((len = ::fread(buf + lenPrev, 1, 65536, pFile)) != 0) {
			len += lenPrev;
			size_t lastLine = 0;
			for (size_t i = 0; i < len; ++ i)
				if (buf[i] == '\r' || buf[i] == '\n') {
					buf[i] = 0;
					char *c = buf + lastLine;
					while (*c == ' ' || *c == '\t')
						++ c;
					//FIXME check the return value and exit on error?
					// Will it break parsing of some obj files?
					obj_parseline(c, data);
					lastLine = i + 1;
				}
			lenPrev = len - lastLine;
			if (lenPrev > 65536) {
		    	BOOST_LOG_TRIVIAL(error) << "ObjParser: Excessive line length";
				::fclose(pFile);
				return false;
			}
			memmove(buf, buf + lastLine, lenPrev);
		}
    }
    catch (std::bad_alloc&) {
    	BOOST_LOG_TRIVIAL(error) << "ObjParser: Out of memory";
	}
	::fclose(pFile);
	return true;
}

bool mtlparse(const char *path, MtlData &data)
{
    Slic3r::CNumericLocalesSetter locales_setter;

    FILE *pFile = boost::nowide::fopen(path, "rt");
    if (pFile == 0) return false;
    cur_mtl_name = "";
    try {
        char   buf[65536 * 2];
        size_t len     = 0;
        size_t lenPrev = 0;
        while ((len = ::fread(buf + lenPrev, 1, 65536, pFile)) != 0) {
            len += lenPrev;
            size_t lastLine = 0;
            for (size_t i = 0; i < len; ++i)
                if (buf[i] == '\r' || buf[i] == '\n') {
                    buf[i]  = 0;
                    char *c = buf + lastLine;
                    while (*c == ' ' || *c == '\t') ++c;
                    // FIXME check the return value and exit on error?
                    // Will it break parsing of some obj files?
                    mtl_parseline(c, data);
                    lastLine = i + 1;
                }
            lenPrev = len - lastLine;
            if (lenPrev > 65536) {
                BOOST_LOG_TRIVIAL(error) << "MtlParser: Excessive line length";
                ::fclose(pFile);
                return false;
            }
            memmove(buf, buf + lastLine, lenPrev);
        }
    } catch (std::bad_alloc &) {
        BOOST_LOG_TRIVIAL(error) << "MtlParser: Out of memory";
    }
    ::fclose(pFile);
    return true;
}

bool objparse(std::istream &stream, ObjData &data)
{
    Slic3r::CNumericLocalesSetter locales_setter;

    try {
        char buf[65536 * 2];
        size_t len = 0;
        size_t lenPrev = 0;
        while ((len = size_t(stream.read(buf + lenPrev, 65536).gcount())) != 0) {
            len += lenPrev;
            size_t lastLine = 0;
            for (size_t i = 0; i < len; ++ i)
                if (buf[i] == '\r' || buf[i] == '\n') {
                    buf[i] = 0;
                    char *c = buf + lastLine;
                    while (*c == ' ' || *c == '\t')
                        ++ c;
                    obj_parseline(c, data);
                    lastLine = i + 1;
                }
            lenPrev = len - lastLine;
            memmove(buf, buf + lastLine, lenPrev);
        }
    }
    catch (std::bad_alloc&) {
    	BOOST_LOG_TRIVIAL(error) << "ObjParser: Out of memory";
    	return false;
    }
    
    return true;
}

template<typename T> 
bool savevector(FILE *pFile, const std::vector<T> &v)
{
	size_t cnt = v.size();
	::fwrite(&cnt, 1, sizeof(cnt), pFile);
	//FIXME sizeof(T) works for data types leaving no gaps in the allocated vector because of alignment of the T type.
	if (! v.empty())
		::fwrite(&v.front(), 1, sizeof(T) * cnt, pFile);
	return true;
}

bool savevector(FILE *pFile, const std::vector<std::string> &v)
{
	size_t cnt = v.size();
	::fwrite(&cnt, 1, sizeof(cnt), pFile);
	for (size_t i = 0; i < cnt; ++ i) {
		size_t len = v[i].size();
		::fwrite(&len, 1, sizeof(cnt), pFile);
		::fwrite(v[i].c_str(), 1, len, pFile);
	}
	return true;
}

template<typename T>
bool savevectornameidx(FILE *pFile, const std::vector<T> &v)
{
	size_t cnt = v.size();
	::fwrite(&cnt, 1, sizeof(cnt), pFile);
	for (size_t i = 0; i < cnt; ++ i) {
		::fwrite(&v[i].vertexIdxFirst, 1, sizeof(int), pFile);
		size_t len = v[i].name.size();
		::fwrite(&len, 1, sizeof(cnt), pFile);
		::fwrite(v[i].name.c_str(), 1, len, pFile);
	}
	return true;
}

template<typename T> 
bool loadvector(FILE *pFile, std::vector<T> &v)
{
	v.clear();
	size_t cnt = 0;
	if (::fread(&cnt, sizeof(cnt), 1, pFile) != 1)
		return false;
	//FIXME sizeof(T) works for data types leaving no gaps in the allocated vector because of alignment of the T type.
	if (cnt != 0) {
		v.assign(cnt, T());
		if (::fread(&v.front(), sizeof(T), cnt, pFile) != cnt)
			return false;
	}
	return true;
}

bool loadvector(FILE *pFile, std::vector<std::string> &v)
{
	v.clear();
	size_t cnt = 0;
	if (::fread(&cnt, sizeof(cnt), 1, pFile) != 1)
		return false;
	v.reserve(cnt);
	for (size_t i = 0; i < cnt; ++ i) {
		size_t len = 0;
		if (::fread(&len, sizeof(len), 1, pFile) != 1)
			return false;
		std::string s(" ", len);
		if (::fread(s.data(), 1, len, pFile) != len)
			return false;
		v.push_back(std::move(s));
	}
	return true;
}

template<typename T>
bool loadvectornameidx(FILE *pFile, std::vector<T> &v)
{
	v.clear();
	size_t cnt = 0;
	if (::fread(&cnt, sizeof(cnt), 1, pFile) != 1)
		return false;
	v.assign(cnt, T());
	for (size_t i = 0; i < cnt; ++ i) {
		if (::fread(&v[i].vertexIdxFirst, sizeof(int), 1, pFile) != 1)
			return false;
		size_t len = 0;
		if (::fread(&len, sizeof(len), 1, pFile) != 1)
			return false;
		v[i].name.assign(" ", len);
		if (::fread(v[i].name.data(), 1, len, pFile) != len)
			return false;
	}
	return true;
}

bool objbinsave(const char *path, const ObjData &data)
{
	FILE *pFile = boost::nowide::fopen(path, "wb");
	if (pFile == 0)
		return false;

	size_t version = 1;
	::fwrite(&version, 1, sizeof(version), pFile);

	bool result =
		savevector(pFile, data.coordinates)			&&
		savevector(pFile, data.textureCoordinates)	&&
		savevector(pFile, data.normals)				&&
		savevector(pFile, data.parameters)			&&
		savevector(pFile, data.mtllibs)				&&
		savevectornameidx(pFile, data.usemtls)		&&
		savevectornameidx(pFile, data.objects)		&&
		savevectornameidx(pFile, data.groups)		&&
		savevector(pFile, data.smoothingGroups)		&&
		savevector(pFile, data.vertices);

	::fclose(pFile);
	return result;
}

bool objbinload(const char *path, ObjData &data)
{
	FILE *pFile = boost::nowide::fopen(path, "rb");
	if (pFile == 0)
		return false;

	data.version = 0;
	if (::fread(&data.version, sizeof(data.version), 1, pFile) != 1)
		return false;
	if (data.version != 1)
		return false;

	bool result =
		loadvector(pFile, data.coordinates)			&&
		loadvector(pFile, data.textureCoordinates)	&&
		loadvector(pFile, data.normals)				&&
		loadvector(pFile, data.parameters)			&&
		loadvector(pFile, data.mtllibs)				&&
		loadvectornameidx(pFile, data.usemtls)		&&
		loadvectornameidx(pFile, data.objects)		&&
		loadvectornameidx(pFile, data.groups)		&&
		loadvector(pFile, data.smoothingGroups)		&&
		loadvector(pFile, data.vertices);

	::fclose(pFile);
	return result;
}

template<typename T>
bool vectorequal(const std::vector<T> &v1, const std::vector<T> &v2)
{
	if (v1.size() != v2.size())
		return false;
	for (size_t i = 0; i < v1.size(); ++ i)
		if (! (v1[i] == v2[i]))
			return false;
	return true;
}

bool vectorequal(const std::vector<std::string> &v1, const std::vector<std::string> &v2)
{
	if (v1.size() != v2.size())
		return false;
	for (size_t i = 0; i < v1.size(); ++ i)
		if (v1[i].compare(v2[i]) != 0)
			return false;
	return true;
}

extern bool objequal(const ObjData &data1, const ObjData &data2)
{
	//FIXME ignore version number
	// version;

	return 
		vectorequal(data1.coordinates,			data2.coordinates)			&&
		vectorequal(data1.textureCoordinates,	data2.textureCoordinates)	&&
		vectorequal(data1.normals,				data2.normals)				&&
		vectorequal(data1.parameters,			data2.parameters)			&&
		vectorequal(data1.mtllibs,				data2.mtllibs)				&&
		vectorequal(data1.usemtls,				data2.usemtls)				&&
		vectorequal(data1.objects,				data2.objects)				&&
		vectorequal(data1.groups,				data2.groups)				&&
		vectorequal(data1.vertices,				data2.vertices);
}

} // namespace ObjParser
