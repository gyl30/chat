CREATE TABLE users (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    username TEXT NOT NULL UNIQUE,
    password_hash VARCHAR(60) NOT NULL CHECK (char_length(password_hash) = 60)
);
