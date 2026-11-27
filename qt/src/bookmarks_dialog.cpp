#include "empty_state.hpp"
#include "theme.hpp"
#include "bookmarks_dialog.hpp"

#include <QAbstractItemView>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QListWidget>
#include <QMenu>
#include <QVBoxLayout>

#include "client_bridge.hpp"
#include "global_search_dialog.hpp"
#include "chat_widget.hpp"
#include "message_preview.hpp"

namespace
{
QString local_time(qint64 milliseconds) { return QDateTime::fromMSecsSinceEpoch(milliseconds).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")); }
}    // namespace

bookmarks_dialog::bookmarks_dialog(client_bridge& client, chat_widget& chat, QWidget* parent)
    : QDialog(parent),
      client_(client),
      chat_(chat),
      query_(new QLineEdit(this)),
      results_(new QListWidget(this)),
      more_(new QPushButton(QStringLiteral("加载更多"), this)),
      status_(new QLabel(this)),
      feedback_(new QLabel(this)),
      refresh_(new QTimer(this))
{
    setObjectName(QStringLiteral("bookmarksDialog"));
    setWindowTitle(QStringLiteral("消息收藏"));
    resize(780, 480);
    query_->setObjectName(QStringLiteral("bookmarkQuery"));
    style_search_field(*query_);
    query_->setPlaceholderText(QStringLiteral("搜索收藏"));
    results_->setObjectName(QStringLiteral("bookmarkResults"));
    results_->setAccessibleName(QStringLiteral("收藏消息"));
    style_message_previews(*results_, *query_, &chat_.avatars());
    more_->setObjectName(QStringLiteral("moreBookmarks"));
    status_->setObjectName(QStringLiteral("bookmarkStatus"));
    feedback_->setTextFormat(Qt::PlainText);
    status_->setTextFormat(Qt::PlainText);
    more_->setAutoDefault(false);

    auto* layout = new QVBoxLayout(this);
    frame_dialog(*this);
    layout->addWidget(query_);
    layout->addWidget(results_, 1);
    empty_ = new empty_state(*results_->viewport(), QStringLiteral("bookmarksEmpty"));
    layout->addWidget(status_);
    layout->addWidget(feedback_);
    layout->addWidget(more_, 0, Qt::AlignHCenter);
    feedback_->hide();

    refresh_->setSingleShot(true);
    refresh_->setInterval(150);
    connect(refresh_, &QTimer::timeout, this, [this] { fetch(0); });
    connect(query_, &QLineEdit::textChanged, this, [this] { refresh(); });
    connect(query_,
            &QLineEdit::returnPressed,
            this,
            [this]
            {
                refresh();
                refresh_->stop();
                fetch(0);
            });
    auto activate = [this](QListWidgetItem* item) { locate(*item); };
    connect(results_, &QListWidget::itemActivated, this, activate);
    connect(results_,
            &QListWidget::itemClicked,
            this,
            [this, activate](QListWidgetItem* item)
            {
                if (!results_->style()->styleHint(QStyle::SH_ItemView_ActivateItemOnSingleClick, nullptr, results_))
                {
                    activate(item);
                }
            });
    results_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(results_, &QListWidget::customContextMenuRequested, this, &bookmarks_dialog::show_menu);
    connect(more_, &QPushButton::clicked, this, [this] { fetch(before_); });
    connect(results_->verticalScrollBar(),
            &QScrollBar::valueChanged,
            this,
            [this](int value)
            {
                if (value > 0 && value == results_->verticalScrollBar()->maximum() && before_ && !request_)
                {
                    fetch(before_);
                }
            });
    connect(&client_, &client_bridge::bookmarks_received, this, &bookmarks_dialog::set_page);
    connect(&client_, &client_bridge::bookmark_changed, this, [this] { refresh(); });
    connect(&client_,
            &client_bridge::bookmark_saved,
            this,
            [this](qint64 message, bool bookmarked, QString const& error)
            {
                if (bookmarked || removing_ != message)
                {
                    return;
                }
                removing_ = 0;
                feedback_->setText(error.isEmpty() ? QStringLiteral("已取消收藏") : error);
                feedback_->show();
                update_actions();
                if (error.isEmpty())
                {
                    refresh();
                }
            });
    connect(&client_,
            &client_bridge::message_updated,
            this,
            [this](qint64, message_data const& message, QString const& error)
            {
                if (error.isEmpty())
                {
                    for (int i = 0; i < results_->count(); ++i)
                    {
                        if (results_->item(i)->data(preview_role::message).toLongLong() == message.id)
                        {
                            refresh();
                            return;
                        }
                    }
                    // An unloaded bookmark can enter a filtered page after an edit. Check ownership
                    // without clearing results for edits to messages the user never bookmarked.
                    if (request_ || !query_->text().isEmpty())
                    {
                        bookmark_checks_.insert(client_.get_message_bookmark(message.id));
                    }
                }
            });
    connect(&client_,
            &client_bridge::bookmark_state_received,
            this,
            [this](quint64 request, bool bookmarked, QString const& error)
            {
                if (bookmark_checks_.remove(request) && error.isEmpty() && bookmarked)
                {
                    refresh();
                }
            });
    connect(&client_,
            &client_bridge::personal_view_changed,
            this,
            [this](qint64, QString const& action)
            {
                if (action == QStringLiteral("clear"))
                {
                    refresh();
                }
            });
    connect(&client_, &client_bridge::conversation_changed, this, [this] { refresh(); });
    connect(&client_, &client_bridge::disconnected, this, &QDialog::reject);
    query_->setFocus(Qt::OtherFocusReason);
    refresh();
}

void bookmarks_dialog::refresh()
{
    bookmark_checks_.clear();
    request_ = 0;
    before_ = 0;
    results_->clear();
    empty_->set_message();
    status_->setText(QStringLiteral("正在加载…"));
    status_->show();
    update_actions();
    refresh_->start();
}

void bookmarks_dialog::fetch(qint64 before)
{
    request_ = client_.list_message_bookmarks(query_->text(), before);
    empty_->set_message();
    status_->setText(QStringLiteral("正在加载…"));
    status_->show();
    update_actions();
}

void bookmarks_dialog::set_page(quint64 request, chat::bookmark_page const& page, QString const& error)
{
    if (request != request_)
    {
        return;
    }
    if (!error.isEmpty())
    {
        request_ = 0;
        status_->setText(error);
        status_->show();
        update_actions();
        return;
    }
    for (auto const& bookmark : page.bookmarks)
    {
        auto* item = new QListWidgetItem(results_);
        auto const saved = QStringLiteral("收藏于 %1").arg(local_time(bookmark.created_at));
        if (auto const& source = bookmark.source)
        {
            auto const summary = bookmark.attachment_type.empty()                 ? QString::fromStdString(source->snippet)
                                 : bookmark.attachment_type.starts_with("image/") ? QStringLiteral("[图片]")
                                                                                  : QStringLiteral("[文件]");
            set_message_preview(*item, *source, summary, saved);
        }
        else
        {
            item->setData(preview_role::message, qint64(bookmark.message));
            item->setData(preview_role::note, saved);
            set_preview_snippet(*item, QStringLiteral("原消息已不可访问"));
        }
    }
    request_ = 0;
    before_ = page.next_before.value_or(0);
    status_->setText(results_->count() ? QString{}
                     : query_->text().isEmpty()    ? QStringLiteral("暂无收藏")
                                                   : QStringLiteral("没有匹配的收藏消息"));
    status_->hide();
    empty_->set_message(status_->text(), u"bookmark");
    update_actions();
}

void bookmarks_dialog::update_actions()
{
    more_->setVisible(before_ != 0);
    more_->setEnabled(before_ && !request_);
}

void bookmarks_dialog::locate(QListWidgetItem const& item)
{
    if (auto const conversation = item.data(preview_role::conversation).toLongLong())
    {
        auto const message = item.data(preview_role::message).toLongLong();
        global_search_dialog context(client_, chat_, this);
        context.locate(conversation, message, [this] { refresh(); }, item.data(preview_role::title).toString());
        context.exec();
    }
}

void bookmarks_dialog::show_menu(QPoint position)
{
    auto const* item = results_->itemAt(position);
    if (!item)
    {
        return;
    }
    auto const message = item->data(preview_role::message).toLongLong();
    QMenu menu(this);
    connect(this, &QDialog::finished, &menu, &QMenu::close);
    auto* open = item->data(preview_role::conversation).toLongLong() ? menu.addAction(QStringLiteral("定位原消息")) : nullptr;
    auto* remove = menu.addAction(QStringLiteral("取消收藏"));
    remove->setEnabled(!removing_);
    auto* selected = menu.exec(results_->viewport()->mapToGlobal(position));
    if (!selected)
    {
        return;
    }
    for (int row = 0; row < results_->count(); ++row)
    {
        auto const& current = *results_->item(row);
        if (current.data(preview_role::message).toLongLong() != message)
        {
            continue;
        }
        if (selected == open)
        {
            locate(current);
        }
        else if (selected == remove && !removing_)
        {
            removing_ = message;
            feedback_->hide();
            client_.set_message_bookmark(message, false);
        }
        break;
    }
}
