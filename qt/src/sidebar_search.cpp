#include "sidebar_search.hpp"

#include "avatar.hpp"
#include "empty_state.hpp"
#include <QLabel>
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
    layout->addWidget(results_, 1);
    empty_ = new empty_state(*results_->viewport(), QStringLiteral("sidebarSearchEmpty"));
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("sidebarSearchStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    status_->setAlignment(Qt::AlignCenter);
    status_->hide();
    layout->addWidget(status_);
    debounce_.setSingleShot(true);
    debounce_.setInterval(220);
    connect(&debounce_, &QTimer::timeout, this, &sidebar_search::search);
    connect(&input_,
            &QLineEdit::textChanged,
            this,
            [this]
            {
                restart();
                refresh_directory();
            });
    connect(&contacts_, &QAbstractItemModel::modelReset, this, &sidebar_search::refresh_directory);
    connect(&rooms_, &QAbstractItemModel::modelReset, this, &sidebar_search::refresh_directory);
    connect(&contacts_,
            &QAbstractItemModel::dataChanged,
            this,
            [this](QModelIndex const&, QModelIndex const&, QList<int> const& roles)
            {
                if (roles.isEmpty() || roles.contains(user_model::search_role) || roles.contains(Qt::DisplayRole))
                {
                    refresh_directory();
                }
            });
    connect(&rooms_,
            &QAbstractItemModel::dataChanged,
            this,
            [this](QModelIndex const&, QModelIndex const&, QList<int> const& roles)
            {
                if (roles.isEmpty() || roles.contains(conversation_model::username_role) || roles.contains(Qt::DisplayRole))
                {
                    refresh_directory();
                }
            });
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
                error_ = error;
                if (!error_.isEmpty() || page.messages.empty())
                {
                    refresh_status();
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
                refresh_status();
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
        refresh_status();
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
    refresh_status();
}
void sidebar_search::restart()
{
    request_ = 0;
    error_.clear();
    debounce_.stop();
    while (results_->count() && !directory_item(*results_->item(results_->count() - 1)))
    {
        delete results_->takeItem(results_->count() - 1);
    }
    if (!input_.text().trimmed().isEmpty())
    {
        debounce_.start();
    }
    refresh_status();
}
void sidebar_search::search()
{
    chat::message_search_filter filter;
    filter.query = input_.text().toStdString();
    request_ = client_.search_all_messages(filter, 0);
    refresh_status();
}

void sidebar_search::refresh_status()
{
    auto const active = !input_.text().trimmed().isEmpty();
    auto const has_rows = results_->count() != 0;
    auto const loading = debounce_.isActive() || request_;
    status_->setText(active && has_rows ? error_ : QString{});
    status_->setVisible(!status_->text().isEmpty());
    if (!active || has_rows)
    {
        empty_->set_message();
    }
    else if (!error_.isEmpty())
    {
        empty_->set_message(error_);
    }
    else if (loading)
    {
        empty_->set_message(QStringLiteral("正在搜索…"));
    }
    else
    {
        empty_->set_message(QStringLiteral("未找到相关结果"), u"search", QStringLiteral("试试其他关键词"));
    }
}
