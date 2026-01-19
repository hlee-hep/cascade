#pragma once

#define CASCADE_VERSION_MAJOR 0
#define CASCADE_VERSION_MINOR 1
#define CASCADE_VERSION_PATCH 1

#define CASCADE_VERSION_STR_HELPER(x) #x
#define CASCADE_VERSION_STR(x) CASCADE_VERSION_STR_HELPER(x)

#define CASCADE_VERSION_STRING                                                                                                                                \
    CASCADE_VERSION_STR(CASCADE_VERSION_MAJOR) "." CASCADE_VERSION_STR(CASCADE_VERSION_MINOR) "." CASCADE_VERSION_STR(CASCADE_VERSION_PATCH)

inline const char *CascadeVersionString() { return CASCADE_VERSION_STRING; }
inline int CascadeVersionMajor() { return CASCADE_VERSION_MAJOR; }
inline int CascadeVersionMinor() { return CASCADE_VERSION_MINOR; }
inline int CascadeVersionPatch() { return CASCADE_VERSION_PATCH; }
