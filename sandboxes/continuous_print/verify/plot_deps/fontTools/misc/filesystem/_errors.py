class FSError(Exception):
    pass


class CreateFailed(FSError):
    pass


class FilesystemClosed(FSError):
    pass


class MissingInfoNamespace(FSError):
    pass


class NoSysPath(FSError):
    pass


class OperationFailed(FSError):
    pass


class IllegalDestination(OperationFailed):
    pass


class ResourceError(FSError):
    pass


class ResourceNotFound(ResourceError):
    pass


class DirectoryExpected(ResourceError):
    pass


class DirectoryNotEmpty(ResourceError):
    pass


class FileExpected(ResourceError):
    pass


class DestinationExists(ResourceError):
    pass


class ResourceReadOnly(ResourceError):
    pass


class IllegalBackReference(ValueError):
    # Named after fs.errors.IllegalBackReference (which is also a ValueError and
    # not an FSError) so that code catching the upstream error keeps working.
    def __init__(self, path):
        super().__init__(f"path {path!r} resolves outside of the filesystem root")
