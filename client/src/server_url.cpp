#include <chat/server_url.hpp>

#include <boost/url/parse.hpp>

namespace chat
{

std::optional<server_url> parse_server_url(std::string_view url)
{
    auto parsed = boost::urls::parse_uri(url);
    if (!parsed || parsed->scheme() != "ws" || !parsed->has_authority() || parsed->host_address().empty() || parsed->has_fragment() ||
        parsed->has_userinfo())
    {
        return std::nullopt;
    }
    server_url value;
    value.host = std::string(parsed->host_address());
    value.port = parsed->has_port() ? std::string(parsed->port()) : std::string{};
    value.host_header = std::string(parsed->encoded_host());
    if (parsed->has_port())
    {
        value.host_header.push_back(':');
        value.host_header.append(parsed->port());
    }
    value.target = std::string(parsed->encoded_target());
    return value;
}

}
