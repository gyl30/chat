#ifndef CHAT_QT_SRC_MESSAGE_MODEL_HPP
#define CHAT_QT_SRC_MESSAGE_MODEL_HPP

#include <QAbstractListModel>
#include <QList>
#include <QString>

#include "message_data.hpp"
#include "member_data.hpp"

class download_tasks;
class avatar_cache;
class message_images;

class message_model final : public QAbstractListModel
{
   public:
    enum role
    {
        id_role = Qt::UserRole + 1,
        from_role,
        timestamp_role,
        text_role,
        markdown_role,
        content_role,
        poll_role,
        outgoing_role,
        sender_name_role,
        forwarded_from_role,
        read_role,
        read_count_role,
        reply_text_role,
        edited_at_role,
        deleted_role,
        attachment_name_role,
        attachment_key_role,
        download_status_role,
        attachment_type_role,
        reactions_role,
        own_reaction_role,
        mentions_role,
        mentioned_role,
        mention_all_role,
        image_role,
        image_status_role,
        client_id_role,
        send_state_role,
        send_error_role,
    };
    enum send_state
    {
        sent,
        sending,
        waiting,
        failed,
    };
    // A message sent from here that the server has not confirmed: listed after the stored messages.
    struct pending_message
    {
        message_data message;
        bool failed = false;
        QString error;
        bool waiting = false;
    };

    explicit message_model(QObject* parent = nullptr, avatar_cache* avatars = nullptr, message_images* images = nullptr);

    int rowCount(QModelIndex const& parent = {}) const override;
    QVariant data(QModelIndex const& index, int role) const override;
    Qt::ItemFlags flags(QModelIndex const& index) const override;

    void set_downloads(download_tasks& downloads);
    void set_self_user(qint64 user);
    void reset(qint64 conversation, bool group = false);
    void set_pending(QList<pending_message> pending);
    void discard_through(qint64 floor);
    // Changes to unloaded rows belong only to the page currently being read.
    void begin_page() { page_changes_.emplace(); }
    void end_page() { page_changes_.reset(); }
    void merge_messages(QList<message_data> messages);
    bool add_message(message_data message);
    void update_message(message_data const& message);
    void set_poll(qint64 message, chat::poll_summary const& summary);
    bool set_reactions(qint64 message, qint64 revision, QList<reaction_data> reactions);
    void set_read_message(qint64 user, qint64 message);
    qint64 read_position(qint64 user) const { return read_positions_.value(user); }
    void set_read_positions(read_positions positions);
    void update_author(qint64 user, QString const& name);
    void update_member_nickname(qint64 user, QString const& nickname);
    QList<member_data> const& members() const { return members_; }
    void set_members(QList<member_data> members);
    QList<member_data> read_members(qint64 message) const;
    bool can_manage_group() const;
    qint64 first_message_id() const;
    qint64 last_message_id() const;

   private:
    download_tasks* downloads_ = nullptr;
    std::optional<QHash<qint64, message_data>> page_changes_;
    void reconcile_page_row(message_data& message) const;
    avatar_cache* avatars_;
    message_images* images_;
    QList<message_data> messages_;
    QList<pending_message> pending_;
    qint64 self_user_ = 0;
    qint64 conversation_ = 0;
    bool group_ = false;
    read_positions read_positions_;
    QList<member_data> members_;
};

#endif
