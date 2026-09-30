CREATE INDEX messages_conversation_idx
    ON messages (sender_id, recipient_id, id DESC);
