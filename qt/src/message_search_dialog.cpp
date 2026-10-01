#include "message_search_dialog.hpp"

#include <QClipboard>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>

#include "message_delegate.hpp"
#include "message_model.hpp"

message_search_dialog::message_search_dialog(qint64 conversation, qint64 self_user, bool group,
                                             QString const& title, QWidget* parent)
    : QDialog(parent), conversation_(conversation), group_(group)
{
    setObjectName(QStringLiteral("messageSearchDialog"));
    setWindowTitle(title + QStringLiteral(" · 搜索消息"));
    resize(700, 600);
    auto* layout = new QVBoxLayout(this);
    auto* search_row = new QHBoxLayout;
    input_ = new QLineEdit(this);
    input_->setObjectName(QStringLiteral("messageSearchEdit"));
    input_->setPlaceholderText(QStringLiteral("输入当前会话的关键词"));
    input_->setMaxLength(256);
    search_button_ = new QPushButton(QStringLiteral("搜索"), this);
    search_button_->setObjectName(QStringLiteral("searchMessagesButton"));
    search_button_->setAutoDefault(false);
    search_row->addWidget(input_, 1);
    search_row->addWidget(search_button_);
    layout->addLayout(search_row);
    status_ = new QLabel(QStringLiteral("不区分大小写；已删除的消息不出现在结果中。"), this);
    status_->setObjectName(QStringLiteral("messageSearchStatus"));
    layout->addWidget(status_);
    messages_ = new message_model(this);
    messages_->set_self_user(self_user);
    messages_->reset(conversation_, group_);
    results_ = new QListView(this);
    results_->setObjectName(QStringLiteral("messageSearchResults"));
    results_->setModel(messages_);
    results_->setItemDelegate(new message_delegate(results_));
    results_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    results_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    results_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(results_, 1);
    more_button_ = new QPushButton(QStringLiteral("更早的结果"), this);
    more_button_->setObjectName(QStringLiteral("moreSearchResultsButton"));
    more_button_->setAutoDefault(false);
    more_button_->setEnabled(false);
    layout->addWidget(more_button_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(search_button_, &QPushButton::clicked, this, [this] { request_search(false); });
    connect(input_, &QLineEdit::returnPressed, this, [this] { request_search(false); });
    connect(more_button_, &QPushButton::clicked, this, [this] { request_search(true); });
    connect(input_, &QLineEdit::textChanged, this, [this] {
        more_button_->setEnabled(next_before_ > 0 && input_->text().trimmed() == query_);
    });
    connect(results_, &QListView::customContextMenuRequested, this, [this](QPoint const& point) {
        auto const index = results_->indexAt(point);
        if (!index.isValid())
        {
            return;
        }
        QMenu menu(this);
        auto* copy = menu.addAction(QStringLiteral("复制消息"));
        if (menu.exec(results_->viewport()->mapToGlobal(point)) == copy)
        {
            QGuiApplication::clipboard()->setText(index.data(message_model::text_role).toString());
        }
    });
}

void message_search_dialog::request_search(bool older)
{
    if (!older)
    {
        query_ = input_->text().trimmed();
        if (query_.isEmpty())
        {
            status_->setText(QStringLiteral("请输入关键词。"));
            return;
        }
        before_ = 0;
        next_before_ = 0;
        messages_->reset(conversation_, group_);
    }
    else
    {
        before_ = next_before_;
    }
    more_button_->setEnabled(false);
    status_->setText(QStringLiteral("正在搜索…"));
    emit search_requested(query_, before_);
}

void message_search_dialog::set_results(qint64 conversation, QString const& query, qint64 before,
                                       QList<message_data> messages, read_positions positions, bool has_more,
                                       QString const& error_message)
{
    if (conversation != conversation_ || query != query_ || before != before_)
    {
        return;
    }
    if (!error_message.isEmpty())
    {
        status_->setText(error_message);
        more_button_->setEnabled(next_before_ > 0 && input_->text().trimmed() == query_);
        return;
    }
    messages_->merge_messages(std::move(messages));
    messages_->set_read_positions(std::move(positions));
    next_before_ = has_more ? messages_->first_message_id() : 0;
    more_button_->setEnabled(next_before_ > 0 && input_->text().trimmed() == query_);
    status_->setText(messages_->rowCount() == 0 ? QStringLiteral("没有匹配的消息。")
                                              : QStringLiteral("找到 %1 条消息").arg(messages_->rowCount()));
    if (before == 0)
    {
        results_->scrollToBottom();
    }
}
