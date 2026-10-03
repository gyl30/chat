BEGIN;

-- Same Unicode White_Space set as SQL 024 and chat::unicode_whitespace.
ALTER TABLE users ADD CONSTRAINT users_username_no_edge_whitespace CHECK (
    username = btrim(username, U&'\0009\000A\000B\000C\000D\0020\0085\00A0\1680\2000\2001\2002\2003\2004\2005\2006\2007\2008\2009\200A\2028\2029\202F\205F\3000')
);

COMMIT;
