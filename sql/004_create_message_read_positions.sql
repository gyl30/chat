CREATE TABLE message_read_positions (
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    peer_user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    last_read_message_id BIGINT NOT NULL,
    PRIMARY KEY (user_id, peer_user_id)
);
