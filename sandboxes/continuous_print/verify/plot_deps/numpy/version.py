
"""
Module to expose more detailed version info for the installed `numpy`
"""
version = "2.5.3"
__version__ = version
full_version = version

git_revision = "dd88c0c19b54ad9ed3533224221285bf0873249a"
release = 'dev' not in version and '+' not in version
short_version = version.split("+")[0]
