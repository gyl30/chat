#include "profile_media.hpp"

#include "chat_widget.hpp"
#include "client_bridge.hpp"
#include "profile_controls.hpp"

#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

namespace
{
struct media_row
{
    chat::message_category category;
    QStringView label;
    QStringView icon;
    QStringView name;
};

constexpr std::array rows{
    media_row{chat::message_category::images, u"图片", u"image", u"profileMediaImages"},
    media_row{chat::message_category::files, u"文件", u"file", u"profileMediaFiles"},
    media_row{chat::message_category::links, u"链接", u"link", u"profileMediaLinks"},
};
}    // namespace

profile_media::profile_media(client_bridge& client, chat_widget& chat, QWidget* parent) : QWidget(parent), client_(client), chat_(chat)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        auto* button = profile_entry(this, rows[i].label.toString(), rows[i].icon);
        button->setObjectName(rows[i].name.toString());
        entries_[i].button = button;
        layout->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, i] { emit history_requested(static_cast<int>(rows[i].category)); });
    }

    refresh_.setSingleShot(true);
    refresh_.setInterval(60);
    connect(&refresh_, &QTimer::timeout, this, &profile_media::request);
    connect(&client, &client_bridge::message_received, this, [this](message_data const& message) { on_message(message, false); });
    connect(&client,
            &client_bridge::send_finished,
            this,
            [this](qint64, qint64, message_data const& message, QString const&, send_status status)
            {
                if (status == send_status::sent)
                {
                    on_message(message, false);
                }
            });
    connect(&client,
            &client_bridge::message_updated,
            this,
            [this](qint64, message_data const& message, QString const& error)
            {
                if (error.isEmpty())
                {
                    on_message(message, true);
                }
            });
    connect(&chat,
            &chat_widget::history_discarded,
            this,
            [this](qint64 conversation, qint64 floor)
            {
                if (conversation != conversation_)
                {
                    return;
                }
                for (auto& entry : entries_)
                {
                    if (entry.head.value_or(0) <= floor)
                    {
                        entry.head.reset();
                        schedule(entry);
                    }
                }
                update_entries();
            });
    connect(&client,
            &client_bridge::conversation_changed,
            this,
            [this](qint64 id, bool removed)
            {
                if (id == conversation_ && removed)
                {
                    set_conversation(0);
                }
            });
    connect(&client, &client_bridge::disconnected, this, &profile_media::reset);
    connect(&client, &client_bridge::login_revoked, this, [this] { set_conversation(0); });
    connect(&client, &client_bridge::logout_finished, this, [this] { set_conversation(0); });
    connect(&client,
            &client_bridge::authentication_finished,
            this,
            [this](bool authenticated)
            {
                if (authenticated && conversation_)
                {
                    for (auto& entry : entries_)
                    {
                        schedule(entry);
                    }
                }
            });
    update_entries();
}

void profile_media::set_conversation(qint64 conversation)
{
    if (conversation_ == conversation)
    {
        return;
    }
    reset();
    conversation_ = conversation;
    if (conversation_)
    {
        for (auto& entry : entries_)
        {
            schedule(entry);
        }
    }
}

void profile_media::reset()
{
    refresh_.stop();
    for (auto& entry : entries_)
    {
        ++entry.revision;
        entry.head.reset();
        entry.dirty = false;
        entry.requesting = false;
    }
    update_entries();
}

void profile_media::schedule(category_entry& entry)
{
    // Invalidate an older snapshot as soon as the relevant event arrives.
    ++entry.revision;
    entry.dirty = true;
    if (!refresh_.isActive())
    {
        refresh_.start();
    }
}

void profile_media::request()
{
    if (!conversation_ || !chat_.connection_available())
    {
        return;
    }
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        auto& entry = entries_[i];
        if (!entry.dirty)
        {
            continue;
        }
        entry.dirty = false;
        entry.requesting = true;
        client_.get_history_head(conversation_,
                                 rows[i].category,
                                 *this,
                                 [this, i, revision = entry.revision](std::expected<qint64, chat::error> result)
                                 {
                                     auto& current = entries_[i];
                                     if (revision != current.revision)
                                     {
                                         return;
                                     }
                                     current.requesting = false;
                                     if (result)
                                     {
                                         current.head = *result;
                                         update_entries();
                                     }
                                     else if (result.error().kind == chat::error_kind::rpc && result.error().code == chat::conversation_unavailable)
                                     {
                                         set_conversation(0);
                                     }
                                 });
    }
}

void profile_media::update_entries()
{
    bool show = false;
    for (auto const& entry : entries_)
    {
        bool const present = entry.head.value_or(0) > 0;
        entry.button->setVisible(present);
        show |= present;
    }
    setVisible(show);
}

void profile_media::on_message(message_data const& message, bool updated)
{
    if (message.conversation != conversation_ || message.id <= chat_.history_floor(conversation_) || message.acknowledgement_only)
    {
        return;
    }
    if (message.deleted)
    {
        for (auto& entry : entries_)
        {
            if (entry.head == message.id || (entry.head.value_or(0) == 0 && entry.requesting))
            {
                if (entry.head == message.id)
                {
                    entry.head.reset();
                }
                schedule(entry);
            }
        }
        update_entries();
        return;
    }

    bool const link = message.text.contains(u"http://") || message.text.contains(u"https://");
    auto& links = entries_[2];
    if (updated && links.head == message.id && !link)
    {
        links.head.reset();
        update_entries();
        schedule(links);
    }
    else if (!links.head.value_or(0) && (link || (updated && links.requesting)))
    {
        schedule(links);
    }
    if (!updated && message.attachment)
    {
        auto& entry = entries_[message.attachment->media_type.startsWith(u"image/") ? 0 : 1];
        if (!entry.head.value_or(0))
        {
            schedule(entry);
        }
    }
}
