#ifndef XAPIAND_INCLUDED_UTILS_H
#define XAPIAND_INCLUDED_UTILS_H

#include <string>

std::string repr(const char *p, size_t size);
std::string repr(const std::string &string);

void log(void *obj, const char *format, ...);

#endif /* XAPIAND_INCLUDED_UTILS_H */
