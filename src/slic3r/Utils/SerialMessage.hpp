#ifndef SERIALMESSAGE_H
#define SERIALMESSAGE_H

#include <string>
#include "SerialMessageType.hpp"

namespace Slic3r { namespace Utils {

    struct SerialMessage
    {
        std::string message;
        enum SerialMessageType messageType;
        SerialMessage(std::string m, SerialMessageType mT)
        {
            message = m;
            messageType = mT;
        }
    };

    
}}
#endif