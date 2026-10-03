BEGIN;

ALTER TABLE users ADD CONSTRAINT users_username_valid CHECK (
    octet_length(username) BETWEEN 1 AND 64
    AND username !~ U&'[\0001-\001F\007F-\009F@\061C\200E\200F\2028-\202E\2066-\2069]'
    AND btrim(username, U&'\0009\000A\000B\000C\000D\0020\0085\00A0\1680\2000\2001\2002\2003\2004\2005\2006\2007\2008\2009\200A\2028\2029\202F\205F\3000') <> ''
);

ALTER TABLE conversations ADD CONSTRAINT conversations_group_title_valid CHECK (
    kind <> 'group' OR (
        octet_length(title) <= 256
        AND btrim(title, U&'\0009\000A\000B\000C\000D\0020\0085\00A0\1680\2000\2001\2002\2003\2004\2005\2006\2007\2008\2009\200A\2028\2029\202F\205F\3000') <> ''
    )
);

COMMIT;
