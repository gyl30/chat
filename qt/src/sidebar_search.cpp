#include "sidebar_search.hpp"

#include "avatar.hpp"
#include "theme_manager.hpp"
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include "client_bridge.hpp"
#include "conversation_model.hpp"
#include "user_model.hpp"
#include <QDateTime>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

namespace
{
constexpr std::size_t preview_count = 5;

class search_result_delegate final : public QStyledItemDelegate
{
   public:
    search_result_delegate(QLineEdit& input, QWidget* parent) : QStyledItemDelegate(parent), input_(input) {}

    void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override
    {
        auto const query = input_.text();
        if (query.isEmpty() || !index.data(Qt::UserRole).isValid())
        {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        QStyleOptionViewItem row(option);
        initStyleOption(&row, index);
        auto* style = row.widget->style();
        auto const rect = style->subElementRect(QStyle::SE_ItemViewItemText, &row, row.widget).adjusted(4, 0, -4, 0);
        auto const lines = row.text.split(u'\n');
        row.text.clear();
        style->drawControl(QStyle::CE_ItemViewItem, &row, painter, row.widget);
        painter->save();
        painter->setClipRect(rect);
        painter->setPen(themed("text"));
        auto y = rect.center().y() - lines.size() * row.fontMetrics.height() / 2;
        for (auto const& line : lines)
        {
            auto visible = row.fontMetrics.elidedText(line, Qt::ElideRight, rect.width());
            QTextLayout text(visible, row.font);
            QList<QTextLayout::FormatRange> matches;
            QTextCharFormat accent;
            accent.setForeground(themed("link"));
            accent.setFontUnderline(true);
            for (qsizetype pos = 0; (pos = visible.indexOf(query, pos, Qt::CaseInsensitive)) >= 0; pos += query.size())
            {
                matches.append({static_cast<int>(pos), static_cast<int>(query.size()), accent});
            }
            text.setFormats(matches);
            text.beginLayout();
            auto layout_line = text.createLine();
            if (layout_line.isValid())
            {
                layout_line.setLineWidth(rect.width());
            }
            text.endLayout();
            text.draw(painter, QPointF(rect.left(), y));
            y += row.fontMetrics.height();
            painter->setPen(themed("text_secondary"));
        }
        painter->restore();
    }

   private:
    QLineEdit& input_;
};
}    // namespace

sidebar_search::sidebar_search(
    client_bridge& client, QLineEdit& input, user_model& contacts, conversation_model& rooms, avatar_cache& avatars, QWidget* parent)
    : QWidget(parent), client_(client), input_(input), contacts_(contacts), rooms_(rooms), avatars_(avatars)
{
    setObjectName(QStringLiteral("sidebarSearchPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 8);
    directory_ = new QListWidget(this);
    directory_->setObjectName(QStringLiteral("searchDirectory"));
    directory_->setItemDelegate(new search_result_delegate(input, directory_));
    directory_->setIconSize(QSize(32, 32));
    layout->addWidget(directory_);
    layout->addWidget(new QLabel(QStringLiteral("聊天记录"), this));
    hits_ = new QListWidget(this);
    hits_->setObjectName(QStringLiteral("sidebarMessageHits"));
    hits_->setItemDelegate(new search_result_delegate(input, hits_));
    layout->addWidget(hits_, 2);
    more_ = new QPushButton(QStringLiteral("更多搜索结果"), this);
    more_->setObjectName(QStringLiteral("sidebarMoreResults"));
    layout->addWidget(more_);
    more_->hide();
    connect(more_, &QPushButton::clicked, this, [this] { emit more_requested(input_.text()); });
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
    connect(&avatars_, &avatar_cache::changed, this, &sidebar_search::refresh_directory);
    auto activate = [this](QListWidgetItem* row)
    {
        auto id = row->data(Qt::UserRole).toLongLong();
        if (!id)
        {
            return;
        }
        if (id < 0)
        {
            emit room_requested(-id);
        }
        else
        {
            emit contact_requested(id, row->data(Qt::UserRole + 1).toString());
        }
    };
    connect(directory_, &QListWidget::itemClicked, this, activate);
    connect(directory_, &QListWidget::itemActivated, this, activate);
    auto locate = [this](QListWidgetItem* row)
    {
        auto const message = row->data(Qt::UserRole + 1).toLongLong();
        if (message)
        {
            emit message_requested(row->data(Qt::UserRole).toLongLong(), message);
        }
    };
    connect(hits_, &QListWidget::itemClicked, this, locate);
    connect(hits_, &QListWidget::itemActivated, this, locate);
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
                    auto* row = new QListWidgetItem(error.isEmpty() ? QStringLiteral("没有找到相关消息") : error, hits_);
                    row->setFlags(Qt::NoItemFlags);
                }
                auto const count = std::min(preview_count, page.messages.size());
                for (std::size_t i = 0; i < count; ++i)
                {
                    auto const& hit = page.messages[i];
                    auto const snippet = QString::fromStdString(hit.snippet).simplified();
                    auto const details = QStringLiteral("%1 · %2 · %3")
                                             .arg(QString::fromStdString(hit.title),
                                                  QString::fromStdString(hit.username),
                                                  QDateTime::fromMSecsSinceEpoch(hit.timestamp).toString(QStringLiteral("MM-dd HH:mm")));
                    auto* row = new QListWidgetItem(snippet + u'\n' + details, hits_);
                    row->setSizeHint(QSize(0, hits_->fontMetrics().height() * 2 + 12));
                    row->setToolTip(row->text());
                    row->setData(Qt::UserRole, qint64(hit.conversation));
                    row->setData(Qt::UserRole + 1, qint64(hit.id));
                }
                more_->setVisible(error.isEmpty() && (page.messages.size() > preview_count || page.next_before.has_value()));
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
                for (int i = 0; i < hits_->count(); ++i)
                {
                    if (hits_->item(i)->data(Qt::UserRole + 1).toLongLong() == msg.id)
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
    directory_->clear();
    auto query = input_.text().trimmed();
    if (query.isEmpty())
    {
        return;
    }
    auto heading = [this](QString const& text)
    {
        auto* item = new QListWidgetItem(text, directory_);
        item->setFlags(Qt::NoItemFlags);
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
        auto* item = new QListWidgetItem(avatar_icon(user.display_name(), 32, avatars_.image(user.id)), user.display_name(), directory_);
        item->setData(Qt::UserRole, user.id);
        item->setData(Qt::UserRole + 1, user.username);
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
        auto* item = new QListWidgetItem(avatar_icon(room.username, 32, avatars_.image(room.avatar_key()), room.id), room.username, directory_);
        item->setData(Qt::UserRole, -room.id);
    }
    directory_->setVisible(directory_->count() > 0);
}
void sidebar_search::restart()
{
    request_ = 0;
    debounce_.stop();
    hits_->clear();
    more_->hide();
    if (!input_.text().isEmpty())
    {
        debounce_.start();
    }
}
void sidebar_search::search()
{
    chat::message_search_filter filter;
    filter.query = input_.text().toStdString();
    request_ = client_.search_all_messages(filter, 0);
    more_->hide();
}
