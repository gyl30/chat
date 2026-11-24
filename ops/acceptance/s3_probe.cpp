#include "curl_handles.hpp"
#include "s3_objects.hpp"
#include "s3_signer.hpp"
#include "s3_xml.hpp"

#include <cassert>
#include <iostream>
#include <string_view>

int main()
{
    auto curl = chat::detail::make_curl();
    assert(curl);
    auto const* info = curl_version_info(CURLVERSION_NOW);
    assert(std::string_view(info->version) == "8.23.0");
    assert(std::string_view(info->ssl_version).starts_with("OpenSSL/"));
    assert((info->features & CURL_VERSION_ASYNCHDNS) && !info->ares && (info->features & CURL_VERSION_IPV6));
    assert(std::string_view(info->protocols[0]) == "http" && std::string_view(info->protocols[1]) == "https" && !info->protocols[2]);
    assert(curl_easy_setopt(curl.get(), CURLOPT_AWS_SIGV4, "aws:amz:us-east-1:s3") == CURLE_OK);

    // AWS's public fixed-clock GET vector uses virtual hosting. Pass its
    // canonical URI to the same signer that takes a path-style URI in Chat.
    // https://docs.aws.amazon.com/AmazonS3/latest/developerguide/sigv4-query-string-auth.html
    s3_settings const aws{
        "examplebucket.s3.amazonaws.com:443", "examplebucket", "AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", "us-east-1", true};
    auto const example = s3::presign(aws, "/test.txt", "GET", {}, 86400, 1369353600);
    assert(example.starts_with("https://examplebucket.s3.amazonaws.com/test.txt?"));
    assert(example.ends_with("&X-Amz-Signature=aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404"));
    assert(s3::path("test", "空格 +/x?") == "/test/%E7%A9%BA%E6%A0%BC%20%2B/x%3F");
    assert(s3::query({{"uploadId", " +/x=&?"}, {"partNumber", "1"}}) == "partNumber=1&uploadId=%20%2B%2Fx%3D%26%3F");
    auto custom = aws;
    custom.endpoint = "[::1]:9000";
    custom.https = false;
    custom.region = "test-region";
    auto const put = s3::presign(custom, s3::path(custom.bucket, "test"), "PUT", {{"uploadId", "+/= ?"}, {"partNumber", "42"}}, 900, 1369353600);
    assert(put.starts_with("http://[::1]:9000/examplebucket/test?"));
    assert(put.contains("%2Ftest-region%2Fs3%2Faws4_request"));
    assert(put.contains("X-Amz-Expires=900") && put.contains("partNumber=42&uploadId=%2B%2F%3D%20%3F"));

    auto const uploads = s3::read_multipart_xml(
        "<?xml version=\"1.0\"?><s:ListMultipartUploadsResult xmlns:s=\"x\">"
        "<Upload><Key>a&amp;&lt;&#x2F;b</Key><UploadId>+&#61;</UploadId></Upload>"
        "<Upload><Key>invalid<Nested/>key</Key><UploadId>unrelated</UploadId></Upload>"
        "<IsTruncated> true </IsTruncated><NextKeyMarker>a&amp;b</NextKeyMarker><NextUploadIdMarker>next</NextUploadIdMarker>"
        "</s:ListMultipartUploadsResult>");
    assert(uploads && uploads->truncated && uploads->next_key == "a&b" && uploads->next_upload == "next");
    assert(uploads->uploads.size() == 2 && uploads->uploads[0].first == "a&</b" && uploads->uploads[0].second == "+=");
    assert(uploads->uploads[1].first.empty());
    auto const parts = s3::read_multipart_xml(
        "<ListPartsResult xmlns=\"x\"><Part><PartNumber>3</PartNumber><Size>16777216</Size><ETag>&quot;abc&quot;</ETag></Part></ListPartsResult>");
    assert(parts && parts->parts.size() == 1 && parts->parts[0].number == 3 && parts->parts[0].size == 16777216 && parts->parts[0].etag == "abc");
    auto const error = s3::read_multipart_xml("\n   <Error><Code>InvalidPart</Code></Error>");
    assert(error && error->root == "Error" && error->error_code == "InvalidPart");
    auto const complete = s3::read_multipart_xml(" \n<?xml version=\"1.0\"?><CompleteMultipartUploadResult/>");
    assert(complete && complete->root == "CompleteMultipartUploadResult");
    assert(!s3::read_multipart_xml("<CompleteMultipartUploadResult><Key>partial"));
    assert(s3::xml_text("a&<>z") == "a&amp;&lt;&gt;z");
    std::cout << "PASS curl " << info->version << " / " << info->ssl_version << ", AWS GET vector, PUT URI, multipart XML\n";
}
