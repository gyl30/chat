#include "sidebar_search.hpp"

#include "avatar.hpp"
#include "message_preview.hpp"
#include "client_bridge.hpp"
#include "conversation_model.hpp"
#include "user_model.hpp"
#include <QListWidget>
#include <QLineEdit>
#include <QVBoxLayout>
#include <algorithm>

namespace
{
constexpr std::size_t preview_count = 5;

bool directory_item(QListWidgetItem const& row)
{
    return row.data(preview_role::directory).isValid() || row.data(preview_role::heading).toInt() == 1;
}
}    // namespace

sidebar_search::sidebar_search(
    client_bridge& client, QLineEdit& input, user_model& contacts, conversation_model& rooms, avatar_cache& avatars, QWidget* parent)
    : QWidget(parent), client_(client), input_(input), contacts_(contacts), rooms_(rooms), avatars_(avatars)
{
    setObjectName(QStringLiteral("sidebarSearchPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 0);
    results_ = new QListWidget(this);
    results_->setObjectName(QStringLiteral("sidebarSearchResults"));
    results_->setAccessibleName(QStringLiteral("好友、群聊和聊天记录搜索结果"));
    style_message_previews(*results_, input, &avatars_, true);
    layout->addWidget(results_);
    debounce_.setSingleShot(true);
    debounce_.setInterval(220);
    connect(&debounce_, &QTimer::timeout, this, &sidebar_search::search);
    connect(&input_,
            &QLineEdit::textChanged,
            this,
            [this]
            {
                refresh_directory();
                restart();
            });
    connect(&contacts_, &QAbstractItemModel::modelReset, this, &sidebar_search::refresh_directory);
    connect(&rooms_, &QAbstractItemModel::modelReset, this, &sidebar_search::refresh_directory);
    auto activate = [this](QListWidgetItem* row)
    {
        auto const id = row->data(preview_role::directory).toLongLong();
        auto const message = row->data(preview_role::message).toLongLong();
        if (id < 0)
        {
            emit room_requested(-id);
        }
        else if (id > 0)
        {
            emit contact_requested(id, row->data(preview_role::sender).toString());
        }
        else if (message)
        {
            emit message_requested(row->data(preview_role::conversation).toLongLong(), message);
        }
        else if (row->data(preview_role::more).toBool())
        {
            emit more_requested(input_.text());
        }
    };
    connect(results_, &QListWidget::itemClicked, this, activate);
    connect(results_, &QListWidget::itemActivated, this, activate);
    connect(&client_,
            &client_bridge::global_search_received,
            this,
            [this](quint64 request, chat::message_search_page const& page, QString const& error)
            {
                if (request != request_)
                {
                    return;
                }
                request_ = 0;
                if (!error.isEmpty() || page.messages.empty())
                {
                    if (!error.isEmpty() || !results_->count())
                    {
                        auto* status = new QListWidgetItem(error.isEmpty() ? QStringLiteral("没有匹配的结果") : error, results_);
                        status->setFlags(Qt::NoItemFlags);
                    }
                    return;
                }
                auto* heading = new QListWidgetItem(QStringLiteral("聊天记录"), results_);
                heading->setData(preview_role::heading, 2);
                heading->setFlags(Qt::NoItemFlags);
                auto const count = std::min(preview_count, page.messages.size());
                for (std::size_t i = 0; i < count; ++i)
                {
                    auto const& hit = page.messages[i];
                    auto* row = new QListWidgetItem;
                    set_message_preview(*row, hit, QString::fromStdString(hit.snippet));
                    results_->addItem(row);
                }
                if (page.messages.size() > preview_count || page.next_before.has_value())
                {
                    auto* more = new QListWidgetItem(QStringLiteral("更多搜索结果"), results_);
                    more->setData(preview_role::more, true);
                }
            });
    // Drop the pending page before a new request can race with a withdrawal or loss of visibility.
    connect(&client_,
            &client_bridge::message_updated,
            this,
            [this](qint64, message_data const& msg, QString const& error)
            {
                if (!error.isEmpty())
                {
                    return;
                }
                if (request_)
                {
                    restart();
                    return;
                }
                for (int i = 0; i < results_->count(); ++i)
                {
                    if (results_->item(i)->data(preview_role::message).toLongLong() == msg.id)
                    {
                        restart();
                        return;
                    }
                }
            });
    connect(&client_,
            &client_bridge::personal_view_changed,
            this,
            [this](qint64, QString const& action, qint64)
            {
                if (action == "clear")
                {
                    restart();
                }
            });
    connect(&client_,
            &client_bridge::conversation_changed,
            this,
            [this](qint64, bool removed)
            {
                if (removed)
                {
                    restart();
                }
            });
    connect(&client_, &client_bridge::disconnected, &input_, &QLineEdit::clear);
}
void sidebar_search::refresh_directory()
{
    while (results_->count() && directory_item(*results_->item(0)))
    {
        delete results_->takeItem(0);
    }
    int next = 0;
    auto query = input_.text().trimmed();
    if (query.isEmpty())
    {
        return;
    }
    auto heading = [this, &next](QString const& text)
    {
        auto* item = new QListWidgetItem(text);
        item->setData(preview_role::heading, 1);
        item->setFlags(Qt::NoItemFlags);
        results_->insertItem(next++, item);
    };
    bool first = true;
    for (int row = 0; row < contacts_.rowCount(); ++row)
    {
        auto index = contacts_.index(row, 0);
        if (!index.data(user_model::search_role).toString().contains(query, Qt::CaseInsensitive))
        {
            continue;
        }
        if (first)
        {
            heading(QStringLiteral("好友"));
            first = false;
        }
        auto const& user = *contacts_.user_at(index);
        auto* item = new QListWidgetItem(user.display_name());
        item->setData(preview_role::directory, user.id);
        item->setData(preview_role::sender, user.username);
        results_->insertItem(next++, item);
    }
    first = true;
    for (auto const& room : rooms_.items())
    {
        if (!room.group || !room.username.contains(query, Qt::CaseInsensitive))
        {
            continue;
        }
        if (first)
        {
            heading(QStringLiteral("群聊"));
            first = false;
        }
        auto* item = new QListWidgetItem(room.username);
        item->setData(preview_role::directory, -room.id);
        results_->insertItem(next++, item);
    }
}
void sidebar_search::restart()
{
    request_ = 0;
    debounce_.stop();
    while (results_->count() && !directory_item(*results_->item(results_->count() - 1)))
    {
        delete results_->takeItem(results_->count() - 1);
    }
    if (!input_.text().trimmed().isEmpty())
    {
        debounce_.start();
    }
}
void sidebar_search::search()
{
    chat::message_search_filter filter;
    filter.query = input_.text().toStdString();
    request_ = client_.search_all_messages(filter, 0);
}
