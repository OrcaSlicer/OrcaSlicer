#include "OverhangOptimization.hpp"
#include "TriangleMesh.hpp"
#include "Model.hpp"
#include "TriangleMeshSlicer.hpp"
//--output-----
//#include <iostream>
//#include <fstream>
//#include <iomanip>


#define EPS 1e-4
#define THRESHOLD 0.02f
namespace Slic3r {

	void OverhangOptimization::init(
        const ModelObject&          object,
        float                       layer_height,
        const std::vector<double>&  layer_height_profile,
        Transform3d                 m_trafo)
	{
		TriangleMesh mesh                   = object.raw_mesh();
        TriangleMesh mesh_copy              = mesh;
		init_layer_height = layer_height;
		input_layer_height = layer_height_profile;
		const ModelInstance &first_instance = *object.instances.front();
		mesh.transform(first_instance.get_matrix(), first_instance.is_left_handed());

		face_layer.reserve(mesh.facets_count());
		for (stl_triangle_vertex_indices face : mesh.its.indices) {
			stl_vertex vertex[3] = { mesh.its.vertices[face[0]], mesh.its.vertices[face[1]], mesh.its.vertices[face[2]] };
			stl_vertex n         = face_normal_normalized(vertex);
			std::pair<float, float> face_z_span {
				std::min(std::min(vertex[0].z(), vertex[1].z()), vertex[2].z()),
				std::max(std::max(vertex[0].z(), vertex[1].z()), vertex[2].z())
			};
			face_layer.emplace_back(Face_Layer({ face_z_span, n }));
			if (face_z_span.first < height_min)
				height_min = face_z_span.first;
			if (face_z_span.second > height_max)
				height_max = face_z_span.second;
		}

		float obj_height = height_max - height_min;
		MeshSlicingParamsEx slicing_params_ex;
		slicing_params_ex.trafo = m_trafo;
		std::vector<float> z_values;
		int layerNum = obj_height / 0.1f;
		for (int i = 0; i < layerNum; i++)
		{
			z_values.push_back((i+1)*0.1f);
		}
		overhang_ratio = get_mesh_overhang(mesh_copy.its, z_values, slicing_params_ex);
       
        //std::ofstream outfile_input("input_overhang.txt");
        for (int i = 0; i < overhang_ratio.size(); i++) {
            if (overhang_ratio[i] > THRESHOLD) 
			{
                is_quick = false;
                break;
			}
           // outfile_input << overhang_ratio[i] << std::endl;
        }
        //outfile_input.close();*/
		//std::sort(face_layer.begin(), face_layer.end(), [](const Face_Layer &f1, const Face_Layer &f2) { return f1.z_span < f2.z_span; });
	}

	std::vector<double> OverhangOptimization::get_layer_height()
	{
        // std::vector<float> result_b;
        float obj_height = height_max - height_min;
        // int layer_n = (int)(obj_height / init_layer_height);
        // result_b.resize(layer_n,init_layer_height);
        std::vector<double> result;
       /* if (is_quick) {
            double first_temp_height = 0;
            while (first_temp_height <= obj_height) {
                result.push_back(first_temp_height);
                result.push_back(init_layer_height);
                first_temp_height += init_layer_height;
            }
            return result;
        }*/
        std::vector<std::pair<float, float>> change_height;

        stl_vertex up_nor = Vec3f(0.f, 0.f, -1.f);
        // 0.3420 cos70
        for (Face_Layer& fl : face_layer) {
            // float arc_zeor = up_nor.dot(Vec3f(0.f,0.f,-1.f));

            float arc = fl.nor.dot(up_nor);
            if (arc < 0.998f && arc >= 0.866f) {
                // change_height.push_back(fl.span);
                // int bot_z = (int)(fl.z_span.first / init_layer_height) ;
                // int up_z = (int)(fl.z_span.second / init_layer_height) ;
                if (change_height.empty()) {
                    change_height.push_back(fl.z_span);
                    continue;
                }

                bool pass = false;
                for (int ci = 0; ci < change_height.size(); ci++) {
                    if (fl.z_span.first >= change_height[ci].first && fl.z_span.first <= change_height[ci].second) {
                        if (fl.z_span.second > change_height[ci].second) {
                            change_height[ci].second = fl.z_span.second;
                            pass                     = true;
                        }
                    }

                    if (fl.z_span.second >= change_height[ci].first && fl.z_span.second <= change_height[ci].second) {
                        if (fl.z_span.first < change_height[ci].first) {
                            change_height[ci].first = fl.z_span.first;
                            pass                    = true;
                        }
                    }

                    if (fl.z_span.first >= change_height[ci].first && fl.z_span.second <= change_height[ci].second)
                        pass = true;

                    if (pass)
                        break;
                }

                if (!pass)
                    change_height.push_back(fl.z_span);

                // for (int i = bot_z; i <= up_z; i++)
                //{
                //	result_b[i] = 0.1f;
                //	//result_b.insert(result_b.begin() + i,0.1f);
                // }
            }
        }

        //---next work
       
#if 0
		bool is_filter = false;
		if (input_layer_height.size() < (obj_height / (10.f * init_layer_height)))
		{
			float print_z = 0.f;
			std::vector<bool> ch_mark(change_height.size(),false);
			while (print_z + EPS < obj_height)
			{
				float current_z = print_z + init_layer_height;
				int layer_z = current_z / 0.1f;				
				//if(1)
			
				if (overhang_ratio[layer_z] > THRESHOLD)
				{
					for (int ci = 0; ci < change_height.size(); ci++)
					{
						if (current_z > change_height[ci].first && current_z < change_height[ci].second)
						{
							if(!ch_mark[ci]/*&&print_z <= change_height[ci].first*/)
							//if (print_z <= change_height[ci].first)
							{
								double sz = (double)(change_height[ci].first - print_z);
								print_z = change_height[ci].first;
								result.push_back(print_z);
								result.push_back(sz);


								while ((print_z + 0.1f + EPS) <= change_height[ci].second)
								{
									print_z += 0.1f;
									result.push_back(print_z);
									result.push_back(0.1f);
								}

								double szz = (double)(change_height[ci].second - print_z);
								print_z = change_height[ci].second;
								result.push_back(print_z);
								result.push_back(szz);
								ch_mark[ci] = true;
							}
						}
					}
				}
				else
				{
					is_filter = true;
				}
				result.push_back(print_z);
				result.push_back(init_layer_height);
				print_z += init_layer_height;
			}
		}
		else
		{
			std::vector<std::pair<int, int>> enter_area;
			for (int ci = 0; ci < change_height.size(); ci++)
			{				
				int begin = -1;
				int end = -1;
				int current_ci = -1;
				for (int i = 0; i < input_layer_height.size(); i += 2)
				{
					if (begin != -1 && current_ci!=-1&&input_layer_height[i]>=change_height[current_ci].second)
					{
						end = i;
					}
					if (begin == -1 && input_layer_height[i] > change_height[ci].first && input_layer_height[i] < change_height[ci].second)
					{						
						begin = i;
						current_ci = ci;
					}
					if (end != -1 && begin != -1)
					{
						break;
					}
				}
				if (end != -1 && begin != -1)
					enter_area.push_back(std::make_pair(begin,end));
			}


			std::sort(enter_area.begin(), enter_area.end(), [&](std::pair<int, int> a, std::pair<int, int> b) { return a.first < b.first; });
			std::vector<bool> mark_h(input_layer_height.size(),false);
			for (int i = 0; i < enter_area.size(); i++)
			{
				for (int ei = enter_area[i].first; ei <= enter_area[i].second; ei++)
				{
					mark_h[ei] = true;
				}
			}

			enter_area.clear();
			int begin = -1;
			int end = -1;
			for (int i = 0; i < mark_h.size(); i++)
			{
				if (i != 0)
				{
					if (mark_h[i] && !mark_h[i - 1])
					{
						begin = i;
					}
					if (!mark_h[i] && mark_h[i - 1])
					{
						end = i-1;
					}
				}
				if (begin != -1 && end != -1)
				{
					enter_area.push_back(std::make_pair(begin,end));
					begin = -1;
					end = -1;
				}
			}

			
			for (int i = 0; i < input_layer_height.size(); i+=1)
			{
				bool is_pass = false;
				for (int ei = 0; ei < enter_area.size(); ei++)
				{
					int f_index = enter_area[ei].first;
					int e_index = enter_area[ei].second;
					if (i >= f_index && i < e_index&&(i%2)==0)
					{						
						//overhang_ratio[layer_z];
						//int index = f_index;
						for (float current_z = input_layer_height[f_index]; current_z < input_layer_height[e_index]; /*current_z += 0.1f*/)
						{			
							int layer_z = current_z / 0.1f;
							if (overhang_ratio[layer_z] < THRESHOLD&&(i%2)==0)
							{								
								//int end_index = input_layer_height[e_index] / 0.1f;
								//int span = end_index - layer_z;
								//float equal_height = input_layer_height[index];
								///*if (span > 0 && span <= 5 && (index % 2) == 0)
								//{
								//	equal_height = 0.1f+(input_layer_height[index+1] - 0.1f) / (span*1.0f);
								//}*/
								//result.push_back(equal_height);
								//result.push_back(input_layer_height[index+1]);
								//current_z += input_layer_height[index+1];
								//current_z -= 0.1f;
								//index += 2;
								
								int qi = -1;
								for (int hi = f_index-2; hi < e_index; hi += 2)
								{
									if (current_z >= input_layer_height[hi] /*&& current_z < input_layer_height[hi + 2]*/)
									{
										qi = hi;
										break;
									}
								}

								result.push_back(current_z);
								result.push_back(input_layer_height[qi+1]);
								current_z += input_layer_height[qi + 1];
								is_filter = true;
								continue;						
							}

							result.push_back(current_z);
							result.push_back(0.1f);
							current_z += 0.1f;
						}
						i = e_index-1;
						is_pass = true;
						break;
					}
				}
				if (!is_pass)
				{
					result.push_back(input_layer_height[i]);
				}
			}
		}
#else
        for (int ci = 0; ci < change_height.size(); ci++) {
            if ((change_height[ci].second - change_height[ci].first) < init_layer_height) {
                change_height.erase(change_height.begin() + ci);
                ci--;
            }
        }

        std::vector<std::pair<int, int>> mark_overhang_beginAndend_index;
        for (int ci = 0; ci < change_height.size(); ci++) {
            int index_b = -1;
            int index_e = -1;
            for (int i = 0; i < input_layer_height.size(); i += 2) {
                if (input_layer_height[i] > (change_height[ci].first - init_layer_height) && input_layer_height[i] < change_height[ci].first) {
                    index_b = i;
                }
                if (input_layer_height[i] > change_height[ci].second && input_layer_height[i] < change_height[ci].second + init_layer_height) {
                    index_e = i;
                }
                if (index_b != -1 && index_e != -1) {
                    mark_overhang_beginAndend_index.push_back(std::make_pair(index_b, index_e));
                    break;
                }
            }
        }

        if (input_layer_height.size() < (obj_height / (10.f * init_layer_height))) {
            float print_z = 0.f;
            while (print_z + EPS < obj_height) {
                bool is_change = false;
                for (int ci = 0; ci < change_height.size(); ci++) {
                    if (print_z > change_height[ci].first - init_layer_height && print_z < change_height[ci].second + init_layer_height) {
                        is_change = true;
                        for (; print_z < change_height[ci].second + init_layer_height; /*print_z += 0.1f*/) {
                            int layer_z = print_z / 0.1f;
                            if (overhang_ratio[layer_z] < THRESHOLD) {
                                result.push_back(print_z);
                                result.push_back(init_layer_height);
                                print_z += init_layer_height;
                                continue;
                            }
                            result.push_back(print_z);
                            result.push_back(0.1f);
                            print_z += 0.1f;
                        }
                    }
                    if (is_change)
                        break;
                }

                if (!is_change) {
                    result.push_back(print_z);
                    result.push_back(init_layer_height);
                    print_z += init_layer_height;
                }
            }			
        } else {
            std::vector<std::pair<int, int>> enter_area;
            for (int ci = 0; ci < change_height.size(); ci++) {
                int begin      = -1;
                int end        = -1;
                int current_ci = -1;
                for (int i = 0; i < input_layer_height.size(); i += 2) {
                    if (begin != -1 && current_ci != -1 && input_layer_height[i] >= change_height[current_ci].second) {
                        end = i;
                    }
                    if (begin == -1 && input_layer_height[i] > change_height[ci].first && input_layer_height[i] < change_height[ci].second) {
                        begin      = i;
                        current_ci = ci;
                    }
                    if (end != -1 && begin != -1) {
                        break;
                    }
                }
                if (end != -1 && begin != -1)
                    enter_area.push_back(std::make_pair(begin, end));
            }

            std::sort(enter_area.begin(), enter_area.end(), [&](std::pair<int, int> a, std::pair<int, int> b) { return a.first < b.first; });
            std::vector<bool> mark_h(input_layer_height.size(), false);
            for (int i = 0; i < enter_area.size(); i++) {
                for (int ei = enter_area[i].first; ei <= enter_area[i].second; ei++) {
                    mark_h[ei] = true;
                }
            }

            enter_area.clear();
            int begin = -1;
            int end   = -1;
            for (int i = 0; i < mark_h.size(); i++) {
                if (i != 0) {
                    if (mark_h[i] && !mark_h[i - 1]) {
                        begin = i;
                    }
                    if (!mark_h[i] && mark_h[i - 1]) {
                        end = i - 1;
                    }
                }
                if (begin != -1 && end != -1) {
                    enter_area.push_back(std::make_pair(begin, end));
                    begin = -1;
                    end   = -1;
                }
            }

            for (int i = 0; i < input_layer_height.size(); i += 1) {
                bool is_pass = false;
                for (int ei = 0; ei < enter_area.size(); ei++) {
                    int f_index = enter_area[ei].first;
                    int e_index = enter_area[ei].second;
                    if (i >= f_index && i < e_index && (i % 2) == 0) {
                        for (float current_z = input_layer_height[f_index]; current_z < input_layer_height[e_index]; /*current_z += 0.1f*/) {
                            int layer_z = current_z / 0.1f;
                            if (overhang_ratio[layer_z] < THRESHOLD && (i % 2) == 0) {
                                int qi = -1;
                                for (int hi = f_index - 2; hi < e_index; hi += 2) {
                                    if (current_z >= input_layer_height[hi] /*&& current_z < input_layer_height[hi + 2]*/) {
                                        qi = hi;
                                        break;
                                    }
                                }

                                result.push_back(current_z);
                                result.push_back(input_layer_height[qi + 1]);
                                current_z += input_layer_height[qi + 1];
                                // is_filter = true;
                                continue;
                            }

                            result.push_back(current_z);
                            result.push_back(0.1f);
                            current_z += 0.1f;
                        }
                        i       = e_index - 1;
                        is_pass = true;
                        break;
                    }
                }
                if (!is_pass) {
                    result.push_back(input_layer_height[i]);
                }
            }
        }

#endif
        return result;
	}
}