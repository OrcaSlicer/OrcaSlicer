# -------------------------------------------------
# qhull-warn.pri -- Qt project warnings for warn_on
#   CONFIG += qhull_warn_all        # Qhull compiles with all warnings except for qhull_warn_shadow and qhull_warn_conversion
#   CONFIG += qhull_warn_conversion # Warn in Qt and Qhull about conversion errors
#   CONFIG += qhull_warn_error      # Turn warnings into errors
#   CONFIG += qhull_warn_shadow     # Warn in Qt about shadowing of functions and fields
# -------------------------------------------------

# [apr'11] VERSION works erratically for msvc builds
# VERSION = 7.2.0
qhull_SOVERSION = 7

# Uncomment to report warnings as errors
#CONFIG += qhull_warn_error

*g++{
    qhull_warn_error{
        QMAKE_CFLAGS_WARN_ON += -Werror
        QMAKE_CXXFLAGS_WARN_ON += -Werror
    }

    QMAKE_CFLAGS_WARN_ON += -Wcast-qual -Wextra -Wshadow -Wwrite-strings

    QMAKE_CXXFLAGS_WARN_ON += -Wcast-qual -Wextra -Wwrite-strings
    QMAKE_CXXFLAGS_WARN_ON += -Wno-sign-conversion

    qhull_warn_shadow{
        QMAKE_CXXFLAGS_WARN_ON += -Wshadow     # Shadowing occurs in Qt, e.g., nested foreach
    }

    qhull_warn_conversion{
        QMAKE_CFLAGS_WARN_ON += -Wno-sign-conversion   # libqhullstatic has many size_t vs. int warnings
        QMAKE_CFLAGS_WARN_ON += -Wconversion           # libqhullstatic has no workaround for bit-field conversions
        QMAKE_CXXFLAGS_WARN_ON += -Wconversion         # Qt has conversion errors for qbitarray and qpalette
    }

    qhull_warn_all{
        QMAKE_CFLAGS_WARN_ON += -Waddress -Warray-bounds -Wchar-subscripts -Wclobbered -Wcomment -Wempty-body
        QMAKE_CFLAGS_WARN_ON += -Wformat -Wignored-qualifiers -Wimplicit-function-declaration -Wimplicit-int
        QMAKE_CFLAGS_WARN_ON += -Wmain -Wmissing-braces -Wmissing-field-initializers -Wmissing-parameter-type
        QMAKE_CFLAGS_WARN_ON += -Wnonnull -Wold-style-declaration -Woverride-init -Wparentheses
        QMAKE_CFLAGS_WARN_ON += -Wpointer-sign -Wreturn-type -Wsequence-point -Wsign-compare
        QMAKE_CFLAGS_WARN_ON += -Wsign-compare -Wstrict-aliasing -Wstrict-overflow=1 -Wswitch
        QMAKE_CFLAGS_WARN_ON += -Wtrigraphs -Wtype-limits -Wuninitialized -Wuninitialized
        QMAKE_CFLAGS_WARN_ON += -Wunknown-pragmas -Wunused-function -Wunused-label -Wunused-parameter
        QMAKE_CFLAGS_WARN_ON += -Wunused-value -Wunused-variable -Wvolatile-register-var

        QMAKE_CXXFLAGS_WARN_ON += -Waddress -Warray-bounds -Wc++0x-compat -Wchar-subscripts
        QMAKE_CXXFLAGS_WARN_ON += -Wclobbered -Wcomment -Wempty-body -Wenum-compare
        QMAKE_CXXFLAGS_WARN_ON += -Wformat -Wignored-qualifiers -Wmain -Wmissing-braces
        QMAKE_CXXFLAGS_WARN_ON += -Wmissing-field-initializers -Wparentheses -Wreorder -Wreturn-type
        QMAKE_CXXFLAGS_WARN_ON += -Wsequence-point -Wsign-compare -Wsign-compare -Wstrict-aliasing
        QMAKE_CXXFLAGS_WARN_ON += -Wstrict-overflow=1 -Wswitch -Wtrigraphs -Wtype-limits
        QMAKE_CXXFLAGS_WARN_ON += -Wuninitialized -Wunknown-pragmas -Wunused-function -Wunused-label
        QMAKE_CXXFLAGS_WARN_ON += -Wunused-parameter -Wunused-value -Wunused-variable -Wvolatile-register-var
    }
}
