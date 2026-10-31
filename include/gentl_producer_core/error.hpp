#pragma once
#include <GenTL.h>
#include <string>
#include <utility>
namespace gentl_producer_core {
void set_last_error(GenTL::GC_ERROR code, std::string text);
std::pair<GenTL::GC_ERROR, std::string> last_error();
} // namespace gentl_producer_core
