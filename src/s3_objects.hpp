#ifndef CHAT_SRC_S3_OBJECTS_HPP
#define CHAT_SRC_S3_OBJECTS_HPP

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

struct s3_settings
{
    std::string endpoint;
    std::string bucket;
    std::string access_key;
    std::string secret_key;
    std::string region;
    bool https = true;
};

struct s3_error
{
    int status;
    std::string detail;
};

// Blocking S3 operations. Only call from an object-storage worker.
class s3_objects
{
   public:
    explicit s3_objects(s3_settings settings);

    // Returns the SHA-256 digest in hexadecimal after S3 validates the checksum.
    std::expected<std::string, s3_error> put(std::string const& key, std::string_view bytes,
                                            std::string_view media_type) const;
    std::expected<std::string, s3_error> read(std::string const& key, std::size_t size,
                                             std::size_t offset, std::size_t length) const;
    std::expected<void, s3_error> remove(std::string const& key) const;

   private:
    s3_settings settings_;
};

#endif
