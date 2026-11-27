#include "theme.hpp"
#include "bookmarks_dialog.hpp"

#include <QAbstractItemView>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "client_bridge.hpp"
#include "global_search_dialog.hpp"

namespace
{
enum
{
    message_role = Qt::UserRole,
    conversation_role,
};

QString local_time(qint64 milliseconds) { return QDateTime::fromMSecsSinceEpoch(milliseconds).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")); }
}    // namespace

bookmarks_dialog::bookmarks_dialog(client_bridge& client, chat_widget& chat, QWidget* parent)
    : QDialog(parent),
      client_(client),
      chat_(chat),
      query_(new QLineEdit(this)),
      results_(new QTreeWidget(this)),
      locate_(new QPushButton(QStringLiteral("定位原消息"), this)),
      remove_(new QPushButton(QStringLiteral("取消收藏"), this)),
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
    query_->setPlaceholderText(QStringLiteral("搜索收藏的消息文本"));
    query_->setClearButtonEnabled(true);
    results_->setObjectName(QStringLiteral("bookmarkResults"));
    results_->setHeaderLabels(
        {QStringLiteral("来源会话"), QStringLiteral("发送者"), QStringLiteral("原消息时间"), QStringLiteral("收藏时间"), QStringLiteral("消息摘要")});
    results_->setRootIsDecorated(false);
    results_->header()->setSectionResizeMode(QHeaderView::Interactive);
    results_->setColumnWidth(0, 145);
    results_->setColumnWidth(1, 95);
    for (int i : {2, 3})
    {
        results_->setColumnWidth(i, 145);
    }
    locate_->setObjectName(QStringLiteral("locateBookmark"));
    remove_->setObjectName(QStringLiteral("removeBookmark"));
    more_->setObjectName(QStringLiteral("moreBookmarks"));
    status_->setObjectName(QStringLiteral("bookmarkStatus"));
    feedback_->setTextFormat(Qt::PlainText);
    status_->setTextFormat(Qt::PlainText);
    locate_->setAutoDefault(false);
    remove_->setAutoDefault(false);
    more_->setAutoDefault(false);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(locate_);
    buttons->addWidget(remove_);
    buttons->addStretch();
    buttons->addWidget(more_);
    auto* layout = new QVBoxLayout(this);
    frame_dialog(*this);
    layout->addWidget(query_);
    layout->addWidget(results_);
    layout->addWidget(status_);
    layout->addWidget(feedback_);
    layout->addLayout(buttons);

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
    connect(results_, &QTreeWidget::itemSelectionChanged, this, &bookmarks_dialog::update_actions);
    connect(results_, &QTreeWidget::itemActivated, this, &bookmarks_dialog::locate);
    connect(locate_, &QPushButton::clicked, this, &bookmarks_dialog::locate);
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
    connect(remove_,
            &QPushButton::clicked,
            this,
            [this]
            {
                auto const* item = results_->currentItem();
                if (!item || removing_)
                {
                    return;
                }
                removing_ = item->data(0, message_role).toLongLong();
                update_actions();
                feedback_->clear();
                client_.set_message_bookmark(removing_, false);
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
                    for (int i = 0; i < results_->topLevelItemCount(); ++i)
                    {
                        if (results_->topLevelItem(i)->data(0, message_role).toLongLong() == message.id)
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
    refresh();
}

void bookmarks_dialog::refresh()
{
    bookmark_checks_.clear();
    request_ = 0;
    before_ = 0;
    results_->clear();
    status_->setText(QStringLiteral("正在加载…"));
    update_actions();
    refresh_->start();
}

void bookmarks_dialog::fetch(qint64 before)
{
    request_ = client_.list_message_bookmarks(query_->text(), before);
    status_->setText(QStringLiteral("正在加载…"));
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
        update_actions();
        return;
    }
    for (auto const& bookmark : page.bookmarks)
    {
        auto* item = new QTreeWidgetItem(results_);
        item->setData(0, message_role, static_cast<qlonglong>(bookmark.message));
        item->setText(3, local_time(bookmark.created_at));
        if (auto const& source = bookmark.source)
        {
            item->setData(0, conversation_role, static_cast<qlonglong>(source->conversation));
            item->setText(0, QString::fromStdString(source->title));
            item->setText(1, QString::fromStdString(source->username));
            item->setText(2, local_time(source->timestamp));
            auto const summary = bookmark.attachment_type.empty()                 ? QString::fromStdString(source->snippet)
                                 : bookmark.attachment_type.starts_with("image/") ? QStringLiteral("[图片]")
                                                                                  : QStringLiteral("[文件]");
            item->setText(4, summary.simplified());
        }
        else
        {
            item->setText(4, QStringLiteral("原消息已不可访问"));
        }
    }
    request_ = 0;
    before_ = page.next_before.value_or(0);
    status_->setText(results_->topLevelItemCount() ? QString{}
                     : query_->text().isEmpty()    ? QStringLiteral("暂无收藏")
                                                   : QStringLiteral("没有匹配的收藏消息"));
    update_actions();
}

void bookmarks_dialog::update_actions()
{
    auto const* item = results_->currentItem();
    locate_->setEnabled(item && item->data(0, conversation_role).toLongLong());
    remove_->setEnabled(item && !removing_);
    more_->setVisible(before_ != 0);
    more_->setEnabled(before_ && !request_);
}

void bookmarks_dialog::locate()
{
    auto const* item = results_->currentItem();
    if (!item)
    {
        return;
    }
    auto const conversation = item->data(0, conversation_role).toLongLong();
    auto const message = item->data(0, message_role).toLongLong();
    if (conversation)
    {
        global_search_dialog context(client_, chat_, this);
        context.locate(conversation, message, [this] { refresh(); }, item->text(0));
        context.exec();
    }
}
