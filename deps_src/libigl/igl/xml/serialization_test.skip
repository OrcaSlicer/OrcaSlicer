//
// Copyright (C) 2014 Christian Schüller <schuellchr@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
//#ifndef IGL_SERIALIZATION_TEST_H
//#define IGL_SERIALIZATION_TEST_H

//#include <igl/Timer.h>
#include "serialize_xml.h"
#include "XMLSerializable.h"

namespace igl
{
  namespace xml
  {
  
    struct Test1111
    {
    };
  
    struct Test1 : public XMLSerializable
    {
      std::string ts;
      std::vector<Test1*> tvt;
      Test1* tt;
  
      Test1()
      {
        tt = NULL;
      }
  
      void InitSerialization()
      {
        Add(ts,"ts",false);
        Add(tvt,"tvt");
        Add(tt,"tt");
      }
    };
  
    struct Test2: public XMLSerializableBase
    {
      char tc;
      int* ti;
      std::vector<short> tvb;
      float tf;
  
      Test2()
      {
        tc = '1';
        ti = NULL;
        tf = 1.0004;
        tvb.push_back(2);
        tvb.push_back(3);
      }
  
      void Serialize(tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element) const
      {
        serialize_xml(tc,"tc",doc,element);
        serialize_xml(ti,"ti",doc,element);
        serialize_xml(tvb,"tvb",doc,element);
      }
      void Deserialize(const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element)
      {
        deserialize_xml(tc,"tc",doc,element);
        deserialize_xml(ti,"ti",doc,element);
        deserialize_xml(tvb,"tvb",doc,element);
      }
      void Serialize(std::vector<char>& buffer) const
      {
        serialize(tc,"tc",buffer);
        serialize(ti,"ti",buffer);
        serialize(tvb,"tvb",buffer);
        serialize(tf,"tf",buffer);
      }
      void Deserialize(const std::vector<char>& buffer)
      {
        deserialize(tc,"tc",buffer);
        deserialize(ti,"ti",buffer);
        deserialize(tvb,"tvb",buffer);
        deserialize(tf,"tf",buffer);
      }
    };
  
    void serialization_test()
    {
      std::string file("test");
  
      bool tbIn = true,tbOut;
      char tcIn = 't',tcOut;
      unsigned char tucIn = 'u',tucOut;
      short tsIn = 6,tsOut;
      int tiIn = -10,tiOut;
      unsigned int tuiIn = 10,tuiOut;
      float tfIn = 1.0005,tfOut;
      double tdIn = 1.000000005,tdOut;
  
      int* tinpIn = NULL,*tinpOut = NULL;
      float* tfpIn = new float,*tfpOut = NULL;
      *tfpIn = 1.11101;
  
      std::string tstrIn("test12345"),tstrOut;
  
      Test2 tObjIn,tObjOut;
      int ti = 2;
      tObjIn.ti = &ti;
  
  
      Test1 test1,test2,test3;
      test1.ts = "100";
      test2.ts = "200";
      test3.ts = "300";
  
      Test1 testA, testC;
      testA.tt = &test1;
      testA.ts = "test123";
      testA.tvt.push_back(&test2);
      testA.tvt.push_back(&test3);
  
      Test1 testB = testA;
      testB.ts = "400";
      testB.tvt.pop_back();
  
      std::pair<int,bool> tPairIn(10,true);
      std::pair<int,bool> tPairOut;
  
      std::vector<int> tVector1In ={1,2,3,4,5};
      std::vector<int> tVector1Out;
  
      std::pair<int,bool> p1(10,1);
      std::pair<int,bool> p2(1,0);
      std::pair<int,bool> p3(10000,1);
      std::vector<std::pair<int,bool> > tVector2In ={p1,p2,p3};
      std::vector<std::pair<int,bool> > tVector2Out;
  
      std::set<std::pair<int,bool> > tSetIn ={p1,p2,p3};
      std::set<std::pair<int,bool> > tSetOut;
  
      std::map<int,bool> tMapIn ={p1,p2,p3};
      std::map<int,bool> tMapOut;
  
      Eigen::Matrix<float,3,3> tDenseMatrixIn;
      tDenseMatrixIn << Eigen::Matrix<float,3,3>::Random();
      tDenseMatrixIn.coeffRef(0,0) = 1.00001;
      Eigen::Matrix<float,3,3> tDenseMatrixOut;
  
      Eigen::Matrix<float,3,3,Eigen::RowMajor> tDenseRowMatrixIn;
      tDenseRowMatrixIn << Eigen::Matrix<float,3,3,Eigen::RowMajor>::Random();
      Eigen::Matrix<float,3,3,Eigen::RowMajor> tDenseRowMatrixOut;
  
      Eigen::SparseMatrix<double> tSparseMatrixIn;
      tSparseMatrixIn.resize(3,3);
      tSparseMatrixIn.insert(0,0) = 1.3;
      tSparseMatrixIn.insert(1,1) = 10.2;
      tSparseMatrixIn.insert(2,2) = 100.1;
      tSparseMatrixIn.finalize();
      Eigen::SparseMatrix<double> tSparseMatrixOut;
  
      // binary serialization
  
      serialize(tbIn,file);
      deserialize(tbOut,file);
      assert(tbIn == tbOut);
  
      serialize(tcIn,file);
      deserialize(tcOut,file);
      assert(tcIn == tcOut);
  
      serialize(tucIn,file);
      deserialize(tucOut,file);
      assert(tucIn == tucOut);
  
      serialize(tsIn,file);
      deserialize(tsOut,file);
      assert(tsIn == tsOut);
  
      serialize(tiIn,file);
      deserialize(tiOut,file);
      assert(tiIn == tiOut);
  
      serialize(tuiIn,file);
      deserialize(tuiOut,file);
      assert(tuiIn == tuiOut);
  
      serialize(tfIn,file);
      deserialize(tfOut,file);
      assert(tfIn == tfOut);
  
      serialize(tdIn,file);
      deserialize(tdOut,file);
      assert(tdIn == tdOut);
  
      serialize(tinpIn,file);
      deserialize(tinpOut,file);
      assert(tinpIn == tinpOut);
  
      serialize(tfpIn,file);
      deserialize(tfpOut,file);
      assert(*tfpIn == *tfpOut);
      tfpOut = NULL;
  
      serialize(tstrIn,file);
      deserialize(tstrOut,file);
      assert(tstrIn == tstrOut);
  
      // updating
      serialize(tbIn,"tb",file,true);
      serialize(tcIn,"tc",file);
      serialize(tiIn,"ti",file);
      tiIn++;
      serialize(tiIn,"ti",file);
      tiIn++;
      serialize(tiIn,"ti",file);
      deserialize(tbOut,"tb",file);
      deserialize(tcOut,"tc",file);
      deserialize(tiOut,"ti",file);
      assert(tbIn == tbOut);
      assert(tcIn == tcOut);
      assert(tiIn == tiOut);
  
      serialize(tsIn,"tsIn",file,true);
      serialize(tVector1In,"tVector1In",file);
      serialize(tVector2In,"tsIn",file);
      deserialize(tVector2Out,"tsIn",file);
      for(unsigned int i=0;i<tVector2In.size();i++)
      {
        assert(tVector2In[i].first == tVector2Out[i].first);
        assert(tVector2In[i].second == tVector2Out[i].second);
      }
      tVector2Out.clear();
  
      serialize(tObjIn,file);
      deserialize(tObjOut,file);
      assert(tObjIn.tc == tObjOut.tc);
      assert(*tObjIn.ti == *tObjOut.ti);
      for(unsigned int i=0;i<tObjIn.tvb.size();i++)
        assert(tObjIn.tvb[i] == tObjOut.tvb[i]);
      tObjOut.ti = NULL;
  
      serialize(tPairIn,file);
      deserialize(tPairOut,file);
      assert(tPairIn.first == tPairOut.first);
      assert(tPairIn.second == tPairOut.second);
  
      serialize(tVector1In,file);
      deserialize(tVector1Out,file);
      for(unsigned int i=0;i<tVector1In.size();i++)
        assert(tVector1In[i] == tVector1Out[i]);
  
      serialize(tVector2In,file);
      deserialize(tVector2Out,file);
      for(unsigned int i=0;i<tVector2In.size();i++)
      {
        assert(tVector2In[i].first == tVector2Out[i].first);
        assert(tVector2In[i].second == tVector2Out[i].second);
      }
  
      serialize(tSetIn,file);
      deserialize(tSetOut,file);
      assert(tSetIn.size() == tSetOut.size());
  
      serialize(tMapIn,file);
      deserialize(tMapOut,file);
      assert(tMapIn.size() == tMapOut.size());
  
      serialize(tDenseMatrixIn,file);
      deserialize(tDenseMatrixOut,file);
      assert((tDenseMatrixIn - tDenseMatrixOut).sum() == 0);
  
      serialize(tDenseRowMatrixIn,file);
      deserialize(tDenseRowMatrixOut,file);
      assert((tDenseRowMatrixIn - tDenseRowMatrixOut).sum() == 0);
  
      serialize(tSparseMatrixIn,file);
      deserialize(tSparseMatrixOut,file);
      assert((tSparseMatrixIn - tSparseMatrixOut).sum() == 0);
  
      serialize(testB,file);
      deserialize(testC,file);
      assert(testB.ts == testC.ts);
      assert(testB.tvt.size() == testC.tvt.size());
      for(unsigned int i=0;i<testB.tvt.size();i++)
      {
        assert(testB.tvt[i]->ts == testC.tvt[i]->ts);
        assert(testB.tvt[i]->tvt.size() == testC.tvt[i]->tvt.size());
        assert(testB.tvt[i]->tt == testC.tvt[i]->tt);
      }
      assert(testB.tt->ts == testC.tt->ts);
      assert(testB.tt->tvt.size() == testC.tt->tvt.size());
      assert(testB.tt->tt == testC.tt->tt);
      testC = Test1();
  
      // big data test
      /*std::vector<std::vector<float> > bigDataIn,bigDataOut;
      for(unsigned int i=0;i<10000;i++)
      {
      std::vector<float> v;
      for(unsigned int j=0;j<10000;j++)
      {
      v.push_back(j);
      }
      bigDataIn.push_back(v);
      }
  
      Timer timer;
      timer.start();
      serialize(bigDataIn,file);
      timer.stop();
      std::cout << "ser: " << timer.getElapsedTimeInMilliSec() << std::endl;
  
      timer.start();
      deserialize(bigDataOut,file);
      timer.stop();
      std::cout << "des: " << timer.getElapsedTimeInMilliSec() << std::endl;
      char c;
      std::cin >> c; */
  
      // xml serialization
  
      serialize_xml(tbIn,file);
      deserialize_xml(tbOut,file);
      assert(tbIn == tbOut);
  
      serialize_xml(tcIn,file);
      deserialize_xml(tcOut,file);
      assert(tcIn == tcOut);
  
      serialize_xml(tucIn,file);
      deserialize_xml(tucOut,file);
      assert(tucIn == tucOut);
  
      serialize_xml(tsIn,file);
      deserialize_xml(tsOut,file);
      assert(tsIn == tsOut);
  
      serialize_xml(tiIn,file);
      deserialize_xml(tiOut,file);
      assert(tiIn == tiOut);
  
      serialize_xml(tuiIn,file);
      deserialize_xml(tuiOut,file);
      assert(tuiIn == tuiOut);
  
      serialize_xml(tfIn,file);
      deserialize_xml(tfOut,file);
      assert(tfIn == tfOut);
  
      serialize_xml(tdIn,file);
      deserialize_xml(tdOut,file);
      assert(tdIn == tdOut);
  
      serialize_xml(tinpIn,file);
      deserialize_xml(tinpOut,file);
      assert(tinpIn == tinpOut);
  
      serialize_xml(tfpIn,file);
      deserialize_xml(tfpOut,file);
      assert(*tfpIn == *tfpOut);
  
      serialize_xml(tstrIn,file);
      deserialize_xml(tstrOut,file);
      assert(tstrIn == tstrOut);
  
      // updating
      serialize_xml(tbIn,"tb",file,false,true);
      serialize_xml(tcIn,"tc",file);
      serialize_xml(tiIn,"ti",file);
      tiIn++;
      serialize_xml(tiIn,"ti",file);
      tiIn++;
      serialize_xml(tiIn,"ti",file);
      deserialize_xml(tbOut,"tb",file);
      deserialize_xml(tcOut,"tc",file);
      deserialize_xml(tiOut,"ti",file);
      assert(tbIn == tbOut);
      assert(tcIn == tcOut);
      assert(tiIn == tiOut);
  
      serialize_xml(tsIn,"tsIn",file,false,true);
      serialize_xml(tVector1In,"tVector1In",file);
      serialize_xml(tVector2In,"tsIn",file);
      deserialize_xml(tVector2Out,"tsIn",file);
      for(unsigned int i=0;i<tVector2In.size();i++)
      {
        assert(tVector2In[i].first == tVector2Out[i].first);
        assert(tVector2In[i].second == tVector2Out[i].second);
      }
      tVector2Out.clear();
  
      // binarization
      serialize_xml(tVector2In,"tVector2In",file,true);
      deserialize_xml(tVector2Out,"tVector2In",file);
      for(unsigned int i=0;i<tVector2In.size();i++)
      {
        assert(tVector2In[i].first == tVector2Out[i].first);
        assert(tVector2In[i].second == tVector2Out[i].second);
      }
  
      serialize_xml(tObjIn,file);
      deserialize_xml(tObjOut,file);
      assert(tObjIn.tc == tObjOut.tc);
      assert(*tObjIn.ti == *tObjOut.ti);
      for(unsigned int i=0;i<tObjIn.tvb.size();i++)
        assert(tObjIn.tvb[i] == tObjOut.tvb[i]);
  
      serialize_xml(tPairIn,file);
      deserialize_xml(tPairOut,file);
      assert(tPairIn.first == tPairOut.first);
      assert(tPairIn.second == tPairOut.second);
  
      serialize_xml(tVector1In,file);
      deserialize_xml(tVector1Out,file);
      for(unsigned int i=0;i<tVector1In.size();i++)
        assert(tVector1In[i] == tVector1Out[i]);
  
      serialize_xml(tVector2In,file);
      deserialize_xml(tVector2Out,file);
      for(unsigned int i=0;i<tVector2In.size();i++)
      {
        assert(tVector2In[i].first == tVector2Out[i].first);
        assert(tVector2In[i].second == tVector2Out[i].second);
      }
  
      serialize_xml(tSetIn,file);
      deserialize_xml(tSetOut,file);
      assert(tSetIn.size() == tSetOut.size());
  
      serialize_xml(tMapIn,file);
      deserialize_xml(tMapOut,file);
      assert(tMapIn.size() == tMapOut.size());
  
      serialize_xml(tDenseMatrixIn,file);
      deserialize_xml(tDenseMatrixOut,file);
      assert((tDenseMatrixIn - tDenseMatrixOut).sum() == 0);
  
      serialize_xml(tDenseRowMatrixIn,file);
      deserialize_xml(tDenseRowMatrixOut,file);
      assert((tDenseRowMatrixIn - tDenseRowMatrixOut).sum() == 0);
  
      serialize_xml(tSparseMatrixIn,file);
      deserialize_xml(tSparseMatrixOut,file);
      assert((tSparseMatrixIn - tSparseMatrixOut).sum() == 0);
  
      serialize_xml(testB,file);
      deserialize_xml(testC,file);
      assert(testB.ts == testC.ts);
      assert(testB.tvt.size() == testC.tvt.size());
      for(unsigned int i=0;i<testB.tvt.size();i++)
      {
        assert(testB.tvt[i]->ts == testC.tvt[i]->ts);
        assert(testB.tvt[i]->tvt.size() == testC.tvt[i]->tvt.size());
        assert(testB.tvt[i]->tt == testC.tvt[i]->tt);
      }
      assert(testB.tt->ts == testC.tt->ts);
      assert(testB.tt->tvt.size() == testC.tt->tvt.size());
      assert(testB.tt->tt == testC.tt->tt);
  
      // big data test
      /*std::vector<std::vector<float> > bigDataIn,bigDataOut;
      for(unsigned int i=0;i<10000;i++)
      {
      std::vector<float> v;
      for(unsigned int j=0;j<10000;j++)
      {
      v.push_back(j);
      }
      bigDataIn.push_back(v);
      }
  
      Timer timer;
      timer.start();
      serialize_xml(bigDataIn,"bigDataIn",file,seRIALIZE_BINARY);
      timer.stop();
      std::cout << "ser: " << timer.getElapsedTimeInMilliSec() << std::endl;
  
      timer.start();
      deserialize_xml(bigDataOut,"bigDataIn",file);
      timer.stop();
      std::cout << "des: " << timer.getElapsedTimeInMilliSec() << std::endl;
      char c;
      std::cin >> c;*/
  
      std::cout << "All tests run successfully!\n";
    }
  }
}

//#endif
