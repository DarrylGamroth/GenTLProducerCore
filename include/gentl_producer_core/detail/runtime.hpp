#pragma once
#include "gentl_producer_core/producer.hpp"
#if defined(GENTL_RUNTIME_COPY)
#include "gentl_producer_core/detail/copy_state.hpp"
#else
#include "gentl_producer_core/detail/queued_state.hpp"
#endif
