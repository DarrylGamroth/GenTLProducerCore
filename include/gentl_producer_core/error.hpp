#pragma once
#include <GenTL.h>
#include <string>
#include <utility>
namespace gentl_producer_core {
// Saves the code and up to 4095 bytes of diagnostic text for this thread.
void set_last_error(GenTL::GC_ERROR code, std::string text);
std::pair<GenTL::GC_ERROR, std::string> last_error();
} // namespace gentl_producer_core
