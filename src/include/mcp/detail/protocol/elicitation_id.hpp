#pragma once

#include <string>

namespace mcp::detail {

/// A fresh, process-unique identifier for a URL-mode elicitation of a legacy session.
std::string generate_elicitation_id();

}  // namespace mcp::detail
