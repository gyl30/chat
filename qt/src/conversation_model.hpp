#ifndef CHAT_QT_SRC_CONVERSATION_MODEL_HPP
#define CHAT_QT_SRC_CONVERSATION_MODEL_HPP

#include <QAbstractListModel>
#include <QList>
#include <QSet>

#include "conversation_data.hpp"

class avatar_cache;

class conversation_model final : public QAbstractListModel
{
   public:
    enum role
    {
        group_role = Qt::UserRole + 1,
        username_role,
        last_text_role,
        last_timestamp_role,
        unread_role,
        unread_mention_role,
        online_role,
        muted_role,
        marked_role,
        pinned_role,
        folder_header_role,
        collapsed_role,
        folder_role,
        avatar_key_role,
    };

    explicit conversation_model(QObject* parent = nullptr, avatar_cache* avatars = nullptr);

    QSet<qint64> const& collapsed() const { return collapsed_; }
    void set_collapsed(QSet<qint64> folders) { collapsed_ = std::move(folders); }
    Qt::ItemFlags flags(QModelIndex const& index) const override;
    int rowCount(QModelIndex const& parent = {}) const override;
    QVariant data(QModelIndex const& index, int role) const override;

    conversation_folder_list const& folders() const { return folders_; }
    QList<conversation_data> const& items() const { return conversations_; }
    void set_folders(conversation_folder_list folders);
    void set_conversations(QList<conversation_data> conversations);
    void discard_through(qint64 conversation, qint64 floor);
    void remove_conversation(qint64 conversation);
    void set_online(qint64 user, bool online);

    void set_notifications(qint64 conversation, chat::notification_level level, qint64 until);
    void set_pinned(qint64 conversation, bool pinned);
    conversation_data const* conversation_at(QModelIndex const& index) const;
    QModelIndex index_for_conversation(qint64 conversation) const;

   private:
    void build_rows();
    conversation_folder_list folders_;
    QSet<qint64> collapsed_;
    // Nonnegative entries index conversations. Negative entries encode folder headings.
    QList<int> rows_;
    avatar_cache* avatars_;
    QList<conversation_data> conversations_;
};

#endif
