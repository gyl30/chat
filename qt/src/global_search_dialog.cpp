#include "global_search_dialog.hpp"

#include <QDateTime>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "client_bridge.hpp"

namespace
{
QString display_time(qint64 ms) { return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("yyyy-MM-dd HH:mm")); }
}    // namespace

global_search_dialog::global_search_dialog(client_bridge& client, QWidget* parent) : QDialog(parent), client_(client)
{
    setWindowTitle(QStringLiteral("全局消息搜索"));
    resize(760, 540);
    auto* layout = new QVBoxLayout(this);
    query_ = new QLineEdit(this);
    query_->setPlaceholderText(QStringLiteral("输入关键词，按回车搜索全部聊天记录"));
    query_->setClearButtonEnabled(true);
    layout->addWidget(query_);
    back_ = new QPushButton(QStringLiteral("返回搜索结果"), this);
    back_->setAutoDefault(false);
    layout->addWidget(back_, 0, Qt::AlignLeft);
    results_ = new QTreeWidget(this);
    results_->setRootIsDecorated(false);
    results_->setHeaderLabels({QStringLiteral("消息摘要"), QStringLiteral("会话"), QStringLiteral("发送者"), QStringLiteral("时间")});
    results_->header()->setStretchLastSection(false);
    results_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    results_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    layout->addWidget(results_);
    context_ = new QListWidget(this);
    context_->setWordWrap(true);
    layout->addWidget(context_);
    status_ = new QLabel(this);
    status_->setTextFormat(Qt::PlainText);
    layout->addWidget(status_);
    more_ = new QPushButton(QStringLiteral("加载更多"), this);
    more_->setAutoDefault(false);
    layout->addWidget(more_, 0, Qt::AlignHCenter);
    show_results();
    connect(query_,
            &QLineEdit::textChanged,
            this,
            [this]
            {
                results_->clear();
                next_before_ = 0;
                show_results();
            });
    connect(query_, &QLineEdit::returnPressed, this, [this] { search(0); });
    connect(more_, &QPushButton::clicked, this, [this] { search(next_before_); });
    connect(back_,
            &QPushButton::clicked,
            this,
            [this]
            {
                if (results_stale_)
                {
                    search(0);
                }
                else
                {
                    show_results();
                }
            });
    connect(results_, &QTreeWidget::itemClicked, this, &global_search_dialog::open_context);
    connect(results_,
            &QTreeWidget::itemActivated,
            this,
            [this](QTreeWidgetItem* item)
            {
                if (!results_->isHidden())
                {
                    open_context(item);
                }
            });
    connect(&client_, &client_bridge::global_search_received, this, &global_search_dialog::set_page);
    connect(&client_, &client_bridge::search_context_received, this, &global_search_dialog::set_context);
    connect(&client_,
            &client_bridge::personal_view_changed,
            this,
            [this](qint64 id, QString const& action)
            {
                if (action == "clear")
                {
                    refresh(id, true);
                }
            });
    connect(&client_, &client_bridge::disconnected, this, &QDialog::reject);
    connect(&client_,
            &client_bridge::conversation_changed,
            this,
            [this](qint64 conv, bool removed)
            {
                if (removed)
                {
                    refresh(conv, true);
                }
            });
    connect(&client_,
            &client_bridge::message_updated,
            this,
            [this](qint64 conv, message_data const&, QString const& error)
            {
                if (error.isEmpty())
                {
                    refresh(conv, false);
                }
            });
}

void global_search_dialog::show_results()
{
    request_ = 0;
    results_stale_ = false;
    setWindowTitle(QStringLiteral("全局消息搜索"));
    context_->clear();
    context_->hide();
    back_->hide();
    query_->show();
    results_->show();
    more_->setVisible(next_before_ != 0);
    status_->clear();
}

void global_search_dialog::search(qint64 before)
{
    show_results();
    if (!before)
    {
        results_->clear();
        next_before_ = 0;
    }
    if (query_->text().isEmpty())
    {
        return;
    }
    status_->setText(QStringLiteral("正在搜索…"));
    more_->hide();
    request_ = client_.search_all_messages(query_->text(), before);
}

void global_search_dialog::open_context(QTreeWidgetItem* item)
{
    auto conv = item->data(0, Qt::UserRole).toLongLong();
    auto anchor = item->data(1, Qt::UserRole).toLongLong();
    setWindowTitle(QStringLiteral("聊天记录 · %1").arg(item->text(1)));
    query_->hide();
    results_->hide();
    more_->hide();
    context_->clear();
    context_->show();
    back_->show();
    status_->setText(QStringLiteral("正在定位消息…"));
    request_ = client_.get_search_context(conv, anchor);
}

void global_search_dialog::set_page(quint64 id, chat::message_search_page const& page, QString const& error)
{
    if (id != request_)
    {
        return;
    }
    status_->setText(error);
    if (error.isEmpty())
    {
        for (auto const& hit : page.messages)
        {
            auto* item = new QTreeWidgetItem(results_,
                                             {QString::fromStdString(hit.snippet),
                                              QString::fromStdString(hit.title),
                                              QString::fromStdString(hit.username),
                                              display_time(hit.timestamp)});
            item->setData(0, Qt::UserRole, static_cast<qlonglong>(hit.conversation));
            item->setData(1, Qt::UserRole, static_cast<qlonglong>(hit.id));
        }
        next_before_ = page.next_before.value_or(0);
        if (!results_->topLevelItemCount())
        {
            status_->setText(QStringLiteral("没有找到相关消息"));
        }
    }
    more_->setVisible(next_before_ != 0);
}
void global_search_dialog::set_context(quint64 id, qint64 anchor, QList<message_data> const& messages, QString const& error)
{
    if (id != request_)
    {
        return;
    }
    if (!error.isEmpty())
    {
        status_->setText(error);
        return;
    }
    status_->clear();
    for (auto const& m : messages)
    {
        context_->addItem(QStringLiteral("%1  %2\n%3").arg(m.username, display_time(m.timestamp), m.deleted ? QStringLiteral("消息已撤回") : m.text));
        if (m.id == anchor)
        {
            context_->setCurrentRow(context_->count() - 1);
        }
    }
    context_->scrollToItem(context_->currentItem(), QAbstractItemView::PositionAtCenter);
}
void global_search_dialog::refresh(qint64 conversation, bool lost_access)
{
    if (context_->isHidden())
    {
        if (request_ || results_->topLevelItemCount())
        {
            search(0);
        }
        return;
    }
    results_stale_ = true;
    auto* current = results_->currentItem();
    if (current && current->data(0, Qt::UserRole).toLongLong() == conversation)
    {
        if (lost_access)
        {
            search(0);
        }
        else
        {
            open_context(current);
        }
    }
}
