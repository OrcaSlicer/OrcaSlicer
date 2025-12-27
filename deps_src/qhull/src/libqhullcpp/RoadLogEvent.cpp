/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/RoadLogEvent.cpp#3 $$Change: 2066 $
** $Date: 2016/01/18 $$Author: bbarber $
**
****************************************************************************/

#//! RoadLogEvent -- All exceptions thrown by Qhull are RoadErrors

#include "libqhullcpp/RoadLogEvent.h"

#include <string>
#include <sstream>
#include <iostream>

using std::cout;
using std::endl;
using std::ostringstream;
using std::string;

#ifdef _MSC_VER  // Microsoft Visual C++ -- warning level 4
#endif

namespace orgQhull {

#//!\name Conversion
string RoadLogEvent::
toString(const char *tag, int code) const
{
    ostringstream os;
    if(tag && code){
        os << tag << code;
        if(format_string){
            os << " ";
        }
    }
    if(!format_string){
        return os.str();
    }
    const char *s= format_string;
    int dCount= 0;  // Count of %d
    int fCount= 0;  // Count of %f
    char extraCode= '\0';
    while(*s){
        if(*s!='%'){
            os << *s++;
        }else{
            char c= *++s;
            s++;
            switch(c){
            case 'd':
                if(++dCount>2){
                    os << " ERROR_three_%d_in_format ";
                }else if(dCount==2){
                    os << int_2;
                }else{
                    os << int_1;
                }
                break;
            case 'e':
                if(firstExtraCode(os, c, &extraCode)){
                    os << double_1;
                }
                break;
            case 'f':
                if(++fCount>1){
                    os << " ERROR_two_%f_in_format ";
                }else{
                    os << float_1;
                }
                break;
            case 'i':
                if(firstExtraCode(os, c, &extraCode)){
                    os << int64_1;
                }
                break;
            case 's':
                if(firstExtraCode(os, c, &extraCode)){
                    os << cstr_1;
                }
                break;
            case 'u':
                if(firstExtraCode(os, c, &extraCode)){
                    os << "0x" << std::hex << int64_1 << std::dec;
                }
                break;
            case 'x':
                if(firstExtraCode(os, c, &extraCode)){
                    os << void_1;
                }
                break;
            case '%':
                os << c;
                break;
            default:
                os << " ERROR_%" << c << "_not_defined_in_format";
                break;
            }
        }
    }
    if(s[-1]!='\n'){
        os << endl;
    }
    return os.str();
}//toString

#//!\name Class helpers (static)

//! True if this char is the first extra code
bool RoadLogEvent::
firstExtraCode(std::ostream &os, char c, char *extraCode){
    if(*extraCode){
        os << " ERROR_%" << *extraCode << "_and_%" << c << "_in_format ";
        return false;
    }
    *extraCode= c;
    return true;
}//firstExtraCode

}//namespace orgQhull

