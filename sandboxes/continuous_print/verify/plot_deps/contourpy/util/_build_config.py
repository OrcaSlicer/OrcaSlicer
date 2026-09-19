# _build_config.py.in is converted into _build_config.py during the meson build process.

from __future__ import annotations


def build_config() -> dict[str, str]:
    """
    Return a dictionary containing build configuration settings.

    All dictionary keys and values are strings, for example ``False`` is
    returned as ``"False"``.

        .. versionadded:: 1.1.0
    """
    return dict(
        # Python settings
        python_version="3.12",
        python_install_dir=r"c:/Lib/site-packages/",
        python_path=r"C:/Users/runneradmin/AppData/Local/Temp/build-env-a3ltv3bn/Scripts/python.exe",

        # Package versions
        contourpy_version="1.4.0",
        meson_version="1.12.0",
        mesonpy_version="0.21.1",
        pybind11_version="3.1.0",

        # Misc meson settings
        meson_backend="ninja",
        build_dir=r"D:/a/contourpy/contourpy/.mesonpy-054es472/lib/contourpy/util",
        source_dir=r"D:/a/contourpy/contourpy/lib/contourpy/util",
        cross_build="False",

        # Build options
        build_options=r"-Dbuildtype=release -Db_ndebug=if-release -Db_vscrt=mt -Dvsenv=true '-Dcpp_link_args=['ucrt.lib','vcruntime.lib','/nodefaultlib:libucrt.lib','/nodefaultlib:libvcruntime.lib']' '--native-file=D:/a/contourpy/contourpy/.mesonpy-054es472/meson-python-native-file.ini'",
        buildtype="release",
        cpp_std="c++17",
        debug="False",
        optimization="3",
        vsenv="True",
        b_ndebug="if-release",
        b_vscrt="mt",

        # C++ compiler
        compiler_name="msvc",
        compiler_version="19.44.35228",
        linker_id="link",
        compile_command="cl",

        # Host machine
        host_cpu="x86_64",
        host_cpu_family="x86_64",
        host_cpu_endian="little",
        host_cpu_system="windows",

        # Build machine, same as host machine if not a cross_build
        build_cpu="x86_64",
        build_cpu_family="x86_64",
        build_cpu_endian="little",
        build_cpu_system="windows",
    )
