// XenonRecomp includes <Windows.h>; the header is windows.h, which only a
// case-insensitive file system forgives. This is what a cross-build finds.
//
// It has to reach the real header, and <windows.h> written here does not: this
// directory is on the include path ahead of the toolchain's, so the name
// resolves back to this file and the recursion ends at the compiler's limit
// with nothing declared at all ("CloseHandle was not declared in this scope",
// on Windows itself). #include_next is the compiler's own "the one after me",
// which is exactly what is meant, and it is a no-op when the system decides to
// find this directory later anyway.
#ifdef __GNUC__
#include_next <windows.h>
#else
#include <windows.h>
#endif
