-- Run offline with sqlite3 -bail, after backing up the database and object storage.
PRAGMA foreign_keys = ON;
BEGIN IMMEDIATE;

-- The new server recreates the message and bundle release triggers at startup.
DROP TRIGGER release_message_attachments;
DROP TRIGGER release_bundle_file;
DROP TRIGGER release_conversation_files;
DROP TABLE conversation_files;

-- Queue objects that no longer have any message or bundle reference.
INSERT INTO attachment_cleanup(object_key,reserved_at)
SELECT f.object_key,0 FROM files f
WHERE NOT EXISTS(SELECT 1 FROM message_attachments m WHERE m.object_key=f.object_key)
  AND NOT EXISTS(SELECT 1 FROM bundle_items b WHERE b.object_key=f.object_key)
  AND NOT EXISTS(SELECT 1 FROM attachment_uploads u WHERE u.object_key=f.object_key)
ON CONFLICT(object_key) DO UPDATE SET reserved_at=0;

ALTER TABLE files DROP COLUMN device;
COMMIT;
