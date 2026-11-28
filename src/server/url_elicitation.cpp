#include <mcp/server/url_elicitation.hpp>

#include "mcp/detail/protocol/elicitation_id.hpp"

#include <utility>

namespace mcp {

UrlElicitationRequired::UrlElicitationRequired(std::vector<ElicitRequestURLParams> elicitations,
                                               const std::string& message)
    : std::runtime_error(message), elicitations_(std::move(elicitations)) {
    for (auto& elicitation : elicitations_) {
        if (!elicitation.elicitationId) {
            elicitation.elicitationId = detail::generate_elicitation_id();
        }
    }
}

}  // namespace mcp
