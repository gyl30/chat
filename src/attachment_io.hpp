#ifndef CHAT_SRC_ATTACHMENT_IO_HPP
#define CHAT_SRC_ATTACHMENT_IO_HPP

#include <cstdint>
#include <string>

#include <boost/capy/task.hpp>

class database;
class s3_objects;

boost::capy::task<bool> discard_upload(database& db, s3_objects& objects, std::int64_t id, std::string const& key, std::string const& multipart);
// Active and resumable uploads retain their cleanup reservations.
// Returns the number removed, or -1 when a cleanup pass could not fully complete.
boost::capy::task<std::int64_t> recover_attachment_objects(database& db, s3_objects& objects, std::int64_t cutoff);

#endif
