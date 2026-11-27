-- Run offline with sqlite3 -bail on each legacy tasks.sqlite3.
PRAGMA foreign_keys = ON;
BEGIN IMMEDIATE;

-- Retire standalone uploads without turning them into messages.
DELETE FROM tasks
WHERE (user,client) IN (SELECT user,client FROM group_file_tasks);
DROP TABLE group_file_tasks;

COMMIT;
