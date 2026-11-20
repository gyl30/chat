#ifndef CHAT_SRC_S3_OBJECTS_HPP
#define CHAT_SRC_S3_OBJECTS_HPP
#include "blocking_worker.hpp"
#include "upload_store.hpp"

struct s3_settings
{
    std::string endpoint, bucket, access_key, secret_key, region;
    bool https = true;
};
struct s3_error
{
    int status;
    std::string_view reason;
};
template <typename T>
using object_result = std::expected<T, s3_error>;

class s3_objects
{
   public:
    explicit s3_objects(s3_settings settings);
    boost::capy::task<object_result<std::string>> initiate(multipart_upload const& task);
    boost::capy::task<object_result<std::string>> signed_url(std::string key, std::string multipart = {}, int part = 0);
    boost::capy::task<object_result<std::optional<std::pair<std::int64_t, std::string>>>> uploaded_part(multipart_upload const& task, int number);
    boost::capy::task<object_result<void>> complete(multipart_upload const& task);
    boost::capy::task<object_result<void>> abort(std::string key, std::string multipart);
    boost::capy::task<object_result<void>> remove(std::string key);

   private:
    s3_settings settings_;
    blocking_worker worker_{8, 2};
};
#endif
