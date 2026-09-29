#include <string>
#include <cstdint>
#include <iostream>
#include <optional>

#include <simdjson.h>

#if !SIMDJSON_STATIC_REFLECTION
#error simdjson static reflection must be enabled
#endif

#if SIMDJSON_EXCEPTIONS
#error simdjson exception interface must be disabled
#endif

namespace
{

struct address
{
    std::string city;
    std::uint64_t zip_code;
};

struct profile
{
    std::uint64_t user_id;
    std::string name;
    address home;
    std::optional<std::string> nickname;
};

template <typename T>
simdjson::error_code parse_json(std::string json, T& value)
{
    simdjson::ondemand::parser parser;
    auto padded = simdjson::pad(json);
    simdjson::ondemand::document document;

    auto error = parser.iterate(padded).get(document);
    if (error)
    {
        return error;
    }

    return document.get(value);
}

template <typename T>
simdjson::error_code serialize_json(T const& value, std::string& json)
{
    return simdjson::builder::to_json_string(value).get(json);
}

bool equal(profile const& lhs, profile const& rhs)
{
    return lhs.user_id == rhs.user_id && lhs.name == rhs.name && lhs.home.city == rhs.home.city && lhs.home.zip_code == rhs.home.zip_code &&
           lhs.nickname == rhs.nickname;
}

int run_tests()
{
    int failures = 0;

    profile original{};
    original.user_id = 7;
    original.name = "alice";
    original.home.city = "Los Angeles";
    original.home.zip_code = 90001;
    original.nickname = "ally";
    std::string json;
    auto error = serialize_json(original, json);
    if (error)
    {
        std::cerr << "FAIL simdjson struct serialization: " << simdjson::error_message(error) << '\n';
        ++failures;
    }
    else
    {
        std::cout << "PASS simdjson struct serialization\n";
    }

    profile decoded{};
    error = parse_json(json, decoded);
    if (error || !equal(original, decoded))
    {
        std::cerr << "FAIL simdjson struct deserialization";
        if (error)
        {
            std::cerr << ": " << simdjson::error_message(error);
        }
        std::cerr << '\n';
        ++failures;
    }
    else
    {
        std::cout << "PASS simdjson struct deserialization\n";
        std::cout << "PASS simdjson nested struct\n";
        std::cout << "PASS simdjson optional value\n";
    }

    profile missing_optional{};
    error = parse_json(R"({"user_id":8,"name":"bob","home":{"city":"Seattle","zip_code":98101}})", missing_optional);
    if (error || missing_optional.nickname.has_value())
    {
        std::cerr << "FAIL simdjson missing optional field\n";
        ++failures;
    }
    else
    {
        std::cout << "PASS simdjson missing optional field\n";
    }

    profile missing_required{};
    error = parse_json(R"({"user_id":9,"name":"carol","nickname":null})", missing_required);
    if (error != simdjson::NO_SUCH_FIELD)
    {
        std::cerr << "FAIL simdjson missing required field: " << simdjson::error_message(error) << '\n';
        ++failures;
    }
    else
    {
        std::cout << "PASS simdjson missing required field\n";
    }

    profile type_mismatch{};
    error = parse_json(R"({"user_id":"10","name":"dave","home":{"city":"Austin","zip_code":78701},"nickname":null})", type_mismatch);
    if (error != simdjson::INCORRECT_TYPE)
    {
        std::cerr << "FAIL simdjson type mismatch: " << simdjson::error_message(error) << '\n';
        ++failures;
    }
    else
    {
        std::cout << "PASS simdjson type mismatch\n";
    }

    profile unknown_field{};
    error = parse_json(
        R"({"user_id":11,"name":"erin","home":{"city":"Denver","zip_code":80202},"nickname":null,"ignored":true})", unknown_field);
    if (error || unknown_field.user_id != 11 || unknown_field.name != "erin")
    {
        std::cerr << "FAIL simdjson unknown field handling\n";
        ++failures;
    }
    else
    {
        std::cout << "PASS simdjson unknown field ignored\n";
    }

    profile malformed{};
    error = parse_json(R"({"user_id":12,"name":"frank")", malformed);
    if (!error)
    {
        std::cerr << "FAIL simdjson malformed JSON error path\n";
        ++failures;
    }
    else
    {
        std::cout << "PASS simdjson malformed JSON error path\n";
    }

    if (failures != 0)
    {
        std::cerr << failures << " simdjson reflection validation test(s) failed\n";
        return 1;
    }

    std::cout << "PASS simdjson reflection validation\n";
    return 0;
}

}    // namespace

int main()
{
    return run_tests();
}
