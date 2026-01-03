// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "readOBJ.h"

#include "list_to_matrix.h"
#include "max_size.h"
#include "min_size.h"
#include "polygon_corners.h"
#include "polygons_to_triangles.h"

#include <iostream>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <iterator>

template <typename Scalar, typename Index>
IGL_INLINE bool igl::readOBJ(
  const std::string obj_file_name,
  std::vector<std::vector<Scalar > > & V,
  std::vector<std::vector<Scalar > > & TC,
  std::vector<std::vector<Scalar > > & N,
  std::vector<std::vector<Index > > & F,
  std::vector<std::vector<Index > > & FTC,
  std::vector<std::vector<Index > > & FN)
{
  // Open file, and check for error
  FILE * obj_file = fopen(obj_file_name.c_str(),"r");
  if(NULL==obj_file)
  {
    fprintf(stderr,"IOError: %s could not be opened...\n",
            obj_file_name.c_str());
    return false;
  }
  std::vector<std::tuple<std::string, Index, Index >> FM;
  return igl::readOBJ(obj_file,V,TC,N,F,FTC,FN, FM);
}

template <typename Scalar, typename Index>
IGL_INLINE bool igl::readOBJ(
  const std::string obj_file_name,
  std::vector<std::vector<Scalar > > & V,
  std::vector<std::vector<Scalar > > & TC,
  std::vector<std::vector<Scalar > > & N,
  std::vector<std::vector<Index > > & F,
  std::vector<std::vector<Index > > & FTC,
  std::vector<std::vector<Index > > & FN,
  std::vector<std::tuple<std::string, Index, Index >> &FM)
{
  // Open file, and check for error
  FILE * obj_file = fopen(obj_file_name.c_str(),"r");
  if(NULL==obj_file)
  {
    fprintf(stderr,"IOError: %s could not be opened...\n",
            obj_file_name.c_str());
    return false;
  }
  return igl::readOBJ(obj_file,V,TC,N,F,FTC,FN,FM);
}

template <typename Scalar, typename Index>
IGL_INLINE bool igl::readOBJ(
  FILE * obj_file,
  std::vector<std::vector<Scalar > > & V,
  std::vector<std::vector<Scalar > > & TC,
  std::vector<std::vector<Scalar > > & N,
  std::vector<std::vector<Index > > & F,
  std::vector<std::vector<Index > > & FTC,
  std::vector<std::vector<Index > > & FN,
  std::vector<std::tuple<std::string, Index, Index >> &FM)
{
  // File open was successful so clear outputs
  V.clear();
  TC.clear();
  N.clear();
  F.clear();
  FTC.clear();
  FN.clear();

  // variables and constants to assist parsing the .obj file
  // Constant strings to compare against
  std::string v("v");
  std::string vn("vn");
  std::string vt("vt");
  std::string f("f");
  std::string tic_tac_toe("#");
#ifndef IGL_LINE_MAX
#  define IGL_LINE_MAX 2048
#endif

#ifndef MATERIAL_LINE_MAX
#  define MATERIAL_LINE_MAX 2048
#endif

  char line[IGL_LINE_MAX];
  char currentmaterialref[MATERIAL_LINE_MAX] = "";
  bool FMwasinit = false;
  int line_no = 1, previous_face_no=0, current_face_no = 0;
  while (fgets(line, IGL_LINE_MAX, obj_file) != NULL)
  {
    char type[IGL_LINE_MAX];
    // Read first word containing type
    if(sscanf(line, "%s",type) == 1)
    {
      // Get pointer to rest of line right after type
      char * l = &line[strlen(type)];
      if(type == v)
      {
        std::istringstream ls(&line[1]);
        std::vector<Scalar > vertex{ std::istream_iterator<Scalar >(ls), std::istream_iterator<Scalar >() };

        if (vertex.size() < 3)
        {
          fprintf(stderr,
                  "Error: readOBJ() vertex on line %d should have at least 3 coordinates",
                  line_no);
          fclose(obj_file);
          return false;
        }
      
        V.push_back(vertex);
      }else if(type == vn)
      {
        double x[3];
        int count =
        sscanf(l,"%lf %lf %lf\n",&x[0],&x[1],&x[2]);
        if(count != 3)
        {
          fprintf(stderr,
                  "Error: readOBJ() normal on line %d should have 3 coordinates",
                  line_no);
          fclose(obj_file);
          return false;
        }
        std::vector<Scalar > normal(count);
        for(int i = 0;i<count;i++)
        {
          normal[i] = x[i];
        }
        N.push_back(normal);
      }else if(type == vt)
      {
        double x[3];
        int count =
        sscanf(l,"%lf %lf %lf\n",&x[0],&x[1],&x[2]);
        if(count != 2 && count != 3)
        {
          fprintf(stderr,
                  "Error: readOBJ() texture coords on line %d should have 2 "
                  "or 3 coordinates (%d)",
                  line_no,count);
          fclose(obj_file);
          return false;
        }
        std::vector<Scalar > tex(count);
        for(int i = 0;i<count;i++)
        {
          tex[i] = x[i];
        }
        TC.push_back(tex);
      }else if(type == f)
      {
        const auto & shift = [&V](const int i)->int
        {
          return i<0 ? i+V.size() : i-1;
        };
        const auto & shift_t = [&TC](const int i)->int
        {
          return i<0 ? i+TC.size() : i-1;
        };
        const auto & shift_n = [&N](const int i)->int
        {
          return i<0 ? i+N.size() : i-1;
        };
        std::vector<Index > face;
        std::vector<Index > ftc;
        std::vector<Index > fn;
        // Read each "word" after type
        char word[IGL_LINE_MAX];
        int offset;
        while(sscanf(l,"%s%n",word,&offset) == 1)
        {
          // adjust offset
          l += offset;
          // Process word
          long int i,it,in;
          if(sscanf(word,"%ld/%ld/%ld",&i,&it,&in) == 3)
          {
            face.push_back(shift(i));
            ftc.push_back(shift_t(it));
            fn.push_back(shift_n(in));
          }else if(sscanf(word,"%ld/%ld",&i,&it) == 2)
          {
            face.push_back(shift(i));
            ftc.push_back(shift_t(it));
          }else if(sscanf(word,"%ld//%ld",&i,&in) == 2)
          {
            face.push_back(shift(i));
            fn.push_back(shift_n(in));
          }else if(sscanf(word,"%ld",&i) == 1)
          {
            face.push_back(shift(i));
          }else
          {
            fprintf(stderr,
                    "Error: readOBJ() face on line %d has invalid element format\n",
                    line_no);
            fclose(obj_file);
            return false;
          }
        }
        if(
           (face.size()>0 && fn.size() == 0 && ftc.size() == 0) ||
           (face.size()>0 && fn.size() == face.size() && ftc.size() == 0) ||
           (face.size()>0 && fn.size() == 0 && ftc.size() == face.size()) ||
           (face.size()>0 && fn.size() == face.size() && ftc.size() == face.size()))
        {
          // No matter what add each type to lists so that lists are the
          // correct lengths
          F.push_back(face);
          FTC.push_back(ftc);
          FN.push_back(fn);
          current_face_no++;
        }else
        {
          fprintf(stderr,
                  "Error: readOBJ() face on line %d has invalid format\n", line_no);
          fclose(obj_file);
          return false;
        }
      }else if(strlen(type) >= 1 && strcmp("usemtl",type)==0 )
      {
        if(FMwasinit){
          FM.push_back(std::make_tuple(currentmaterialref,previous_face_no,current_face_no-1));
          previous_face_no = current_face_no;
        }
        else{
          FMwasinit=true;
        }
        sscanf(l, "%s\n", currentmaterialref);
      }
      else if(strlen(type) >= 1 && (type[0] == '#' ||
            type[0] == 'g'  ||
            type[0] == 's'  ||
            strcmp("mtllib",type)==0))
      {
        //ignore comments or other shit
      }else
      {
        //ignore any other lines
        fprintf(stderr,
                "Warning: readOBJ() ignored non-comment line %d:\n  %s",
                line_no,
                line);
      }
    }else
    {
      // ignore empty line
    }
    line_no++;
  }
  if(strcmp(currentmaterialref,"")!=0)
    FM.push_back(std::make_tuple(currentmaterialref,previous_face_no,current_face_no-1));
  fclose(obj_file);

  assert(F.size() == FN.size());
  assert(F.size() == FTC.size());

  return true;
}

template <typename Scalar, typename Index>
IGL_INLINE bool igl::readOBJ(
  const std::string obj_file_name,
  std::vector<std::vector<Scalar > > & V,
  std::vector<std::vector<Index > > & F)
{
  std::vector<std::vector<Scalar > > TC,N;
  std::vector<std::vector<Index > > FTC,FN;
  std::vector<std::tuple<std::string, Index, Index >> FM;
  
  return readOBJ(obj_file_name,V,TC,N,F,FTC,FN);
}

template <
  typename DerivedV, 
  typename DerivedTC, 
  typename DerivedCN, 
  typename DerivedF,
  typename DerivedFTC,
  typename DerivedFN>
IGL_INLINE bool igl::readOBJ(
  const std::string str,
  Eigen::PlainObjectBase<DerivedV>& V,
  Eigen::PlainObjectBase<DerivedTC>& TC,
  Eigen::PlainObjectBase<DerivedCN>& CN,
  Eigen::PlainObjectBase<DerivedF>& F,
  Eigen::PlainObjectBase<DerivedFTC>& FTC,
  Eigen::PlainObjectBase<DerivedFN>& FN)
{
  std::vector<std::vector<double> > vV,vTC,vN;
  std::vector<std::vector<int> > vF,vFTC,vFN;
  bool success = igl::readOBJ(str,vV,vTC,vN,vF,vFTC,vFN);
  if(!success)
  {
    // readOBJ(str,vV,vTC,vN,vF,vFTC,vFN) should have already printed an error
    // message to stderr
    return false;
  }
  bool V_rect = igl::list_to_matrix(vV,V);
  const char * format = "Failed to cast %s to matrix: min (%d) != max (%d)\n";
  if(!V_rect)
  {
    printf(format,"V",igl::min_size(vV),igl::max_size(vV));
    return false;
  }
  bool F_rect = igl::list_to_matrix(vF,F);
  if(!F_rect)
  {
    printf(format,"F",igl::min_size(vF),igl::max_size(vF));
    return false;
  }
  if(!vN.empty())
  {
    bool VN_rect = igl::list_to_matrix(vN,CN);
    if(!VN_rect)
    {
      printf(format,"CN",igl::min_size(vN),igl::max_size(vN));
      return false;
    }
  }

  if(!vFN.empty() && !vFN[0].empty())
  {
    bool FN_rect = igl::list_to_matrix(vFN,FN);
    if(!FN_rect)
    {
      printf(format,"FN",igl::min_size(vFN),igl::max_size(vFN));
      return false;
    }
  }

  if(!vTC.empty())
  {

    bool T_rect = igl::list_to_matrix(vTC,TC);
    if(!T_rect)
    {
      printf(format,"TC",igl::min_size(vTC),igl::max_size(vTC));
      return false;
    }
  }
  if(!vFTC.empty()&& !vFTC[0].empty())
  {

    bool FTC_rect = igl::list_to_matrix(vFTC,FTC);
    if(!FTC_rect)
    {
      printf(format,"FTC",igl::min_size(vFTC),igl::max_size(vFTC));
      return false;
    }
  }
  return true;
}

template <typename DerivedV, typename DerivedF>
IGL_INLINE bool igl::readOBJ(
  const std::string str,
  Eigen::PlainObjectBase<DerivedV>& V,
  Eigen::PlainObjectBase<DerivedF>& F)
{
  std::vector<std::vector<double> > vV,vTC,vN;
  std::vector<std::vector<int> > vF,vFTC,vFN;
  bool success = igl::readOBJ(str,vV,vTC,vN,vF,vFTC,vFN);
  if(!success)
  {
    // readOBJ(str,vV,vTC,vN,vF,vFTC,vFN) should have already printed an error
    // message to stderr
    return false;
  }
  bool V_rect = igl::list_to_matrix(vV,V);
  if(!V_rect)
  {
    // igl::list_to_matrix(vV,V) already printed error message to std err
    return false;
  }
  bool F_rect = igl::list_to_matrix(vF,F);
  if(!F_rect)
  {
    // igl::list_to_matrix(vF,F) already printed error message to std err
    return false;
  }
  return true;
}

template <typename DerivedV, typename DerivedI, typename DerivedC>
IGL_INLINE bool igl::readOBJ(
  const std::string str,
  Eigen::PlainObjectBase<DerivedV>& V,
  Eigen::PlainObjectBase<DerivedI>& I,
  Eigen::PlainObjectBase<DerivedC>& C)
{
  // we should flip this so that the base implementation uses arrays.
  std::vector<std::vector<double> > vV,vTC,vN;
  std::vector<std::vector<int> > vF,vFTC,vFN;
  bool success = igl::readOBJ(str,vV,vTC,vN,vF,vFTC,vFN);
  if(!success)
  {
    // readOBJ(str,vV,vTC,vN,vF,vFTC,vFN) should have already printed an error
    // message to stderr
    return false;
  }
  if(!igl::list_to_matrix(vV,V))
  {
    return false;
  }
  igl::polygon_corners(vF,I,C);
  return true;
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template bool igl::readOBJ<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template bool igl::readOBJ<double, int>(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::vector<std::vector<double, std::allocator<double> >, std::allocator<std::vector<double, std::allocator<double> > > >&, std::vector<std::vector<double, std::allocator<double> >, std::allocator<std::vector<double, std::allocator<double> > > >&, std::vector<std::vector<double, std::allocator<double> >, std::allocator<std::vector<double, std::allocator<double> > > >&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >&);
template bool igl::readOBJ<double, int>(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::vector<std::vector<double, std::allocator<double> >, std::allocator<std::vector<double, std::allocator<double> > > >&, std::vector<std::vector<double, std::allocator<double> >, std::allocator<std::vector<double, std::allocator<double> > > >&, std::vector<std::vector<double, std::allocator<double> >, std::allocator<std::vector<double, std::allocator<double> > > >&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >&, std::vector<std::tuple<std::basic_string<char, std::char_traits<char>, std::allocator<char> >, int, int>, std::allocator<std::tuple<std::basic_string<char, std::char_traits<char>, std::allocator<char> >, int, int> > >&);
template bool igl::readOBJ<double, int>(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::vector<std::vector<double, std::allocator<double> >, std::allocator<std::vector<double, std::allocator<double> > > >&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >&);
template bool igl::readOBJ<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template bool igl::readOBJ<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 1, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template bool igl::readOBJ<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template bool igl::readOBJ<Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 1, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 1, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template bool igl::readOBJ<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<float, -1, 2, 1, -1, 2>, Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<unsigned int, -1, 3, 1, -1, 3>, Eigen::Matrix<unsigned int, -1, 3, 1, -1, 3>, Eigen::Matrix<unsigned int, -1, 3, 1, -1, 3> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 2, 1, -1, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<unsigned int, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<unsigned int, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<unsigned int, -1, 3, 1, -1, 3> >&);
#endif
