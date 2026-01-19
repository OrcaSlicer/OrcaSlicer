#ifndef slic3r_UserNotification_hpp_
#define slic3r_UserNotification_hpp_


namespace Slic3r {

enum class UserNotificationStyle {
    UNS_NORMAL,
    UNS_WARNING_CONFIRM,
};

class UserNotification
{
public:
    UserNotification() {}
};

} // namespace Slic3r

#endif
