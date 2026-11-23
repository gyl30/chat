#include "global_search_dialog.hpp"

#include <QDateTime>
#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QGridLayout>
#include <limits>
#include <algorithm>
#include <utf8proc.h>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <QSignalBlocker>
#include "client_bridge.hpp"

namespace
{
QString display_time(qint64 ms) { return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("yyyy-MM-dd HH:mm")); }
QString search_snippet(QString const& body, QString const& query)
{
    if (query.isEmpty())
    {
        auto const points = body.toStdU32String();
        return QString::fromUcs4(points.data(), std::min<std::size_t>(160, points.size()));
    }
    auto const original = body.toStdU32String();
    auto const lower = [](auto points)
    {
        for (auto& ch : points)
        {
            ch = utf8proc_tolower(ch);
        }
        return points;
    };
    auto const text = lower(original);
    auto const needle = lower(query.toStdU32String());
    auto const match = std::ranges::search(text, needle);
    if (match.empty())
    {
        return {};
    }
    auto const start = std::max<qsizetype>(0, (match.begin() - text.begin()) - 32);
    return QString::fromUcs4(original.data() + start, std::min<qsizetype>(160, std::ssize(original) - start));
}
}    // namespace

global_search_dialog::global_search_dialog(client_bridge& client, QWidget* parent, qint64 self, QString const& self_name, bool mentions_me)
    : QDialog(parent), client_(client), self_(self)
{
    setWindowTitle(QStringLiteral("全局消息搜索"));
    resize(760, 540);
    auto* layout = new QVBoxLayout(this);
    query_ = new QLineEdit(this);
    query_->setPlaceholderText(QStringLiteral("输入关键词，按回车搜索全部聊天记录"));
    query_->setObjectName(QStringLiteral("globalSearchQuery"));
    query_->setClearButtonEnabled(true);
    layout->addWidget(query_);
    filters_ = new QWidget(this);
    auto* fields = new QGridLayout(filters_);
    fields->setContentsMargins(0, 0, 0, 0);
    conversation_ = new QComboBox(filters_);
    conversation_->setObjectName(QStringLiteral("searchConversation"));
    conversation_->addItem(QStringLiteral("全部会话"));
    conversation_->setEnabled(false);
    category_ = new QComboBox(filters_);
    category_->setObjectName(QStringLiteral("searchCategory"));
    category_->addItems({QStringLiteral("全部类别"), QStringLiteral("图片"), QStringLiteral("文件"), QStringLiteral("链接")});
    sender_ = new QComboBox(filters_);
    sender_->setObjectName(QStringLiteral("searchSender"));
    sender_->addItem(QStringLiteral("全部发送者"));
    if (self > 0)
    {
        sender_->addItem(QStringLiteral("%1（我）").arg(self_name), self);
    }
    auto* sender_name = new QLineEdit(filters_);
    sender_name->setPlaceholderText(QStringLiteral("查找发送者用户名"));
    auto* find_sender = new QPushButton(QStringLiteral("查找用户"), filters_);
    auto* submit = new QPushButton(QStringLiteral("搜索"), filters_);
    find_sender->setAutoDefault(false);
    submit->setAutoDefault(false);
    start_enabled_ = new QCheckBox(QStringLiteral("起始日"), filters_);
    end_enabled_ = new QCheckBox(QStringLiteral("结束日"), filters_);
    mentions_me_ = new QCheckBox(QStringLiteral("提及我"), filters_);
    mentions_me_->setObjectName(QStringLiteral("searchMentionsMe"));
    mentions_me_->setChecked(mentions_me);
    connect(mentions_me_, &QCheckBox::toggled, this, &global_search_dialog::filter_changed);
    start_date_ = new QDateEdit(QDate::currentDate(), filters_);
    end_date_ = new QDateEdit(QDate::currentDate(), filters_);
    for (auto* date : {start_date_, end_date_})
    {
        date->setCalendarPopup(true);
        date->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
        date->setEnabled(false);
        date->setToolTip(QStringLiteral("按系统本地时区，包含选定日期"));
        connect(date, &QDateEdit::dateChanged, this, &global_search_dialog::filter_changed);
    }
    connect(start_enabled_, &QCheckBox::toggled, start_date_, &QDateEdit::setEnabled);
    connect(end_enabled_, &QCheckBox::toggled, end_date_, &QDateEdit::setEnabled);
    connect(start_enabled_, &QCheckBox::toggled, this, &global_search_dialog::filter_changed);
    connect(end_enabled_, &QCheckBox::toggled, this, &global_search_dialog::filter_changed);
    fields->addWidget(conversation_, 0, 0);
    fields->addWidget(category_, 0, 1);
    fields->addWidget(sender_, 0, 2);
    fields->addWidget(sender_name, 0, 3);
    fields->addWidget(find_sender, 0, 4);
    fields->addWidget(start_enabled_, 1, 0);
    fields->addWidget(start_date_, 1, 1);
    fields->addWidget(end_enabled_, 1, 2);
    fields->addWidget(end_date_, 1, 3);
    fields->addWidget(mentions_me_, 2, 0, 1, 2);
    fields->addWidget(submit, 2, 4);
    layout->addWidget(filters_);
    for (auto* box : {conversation_, category_, sender_})
    {
        connect(box, &QComboBox::currentIndexChanged, this, &global_search_dialog::filter_changed);
    }
    auto lookup_sender = [this, sender_name]
    {
        auto const name = sender_name->text().trimmed();
        if (!name.isEmpty())
        {
            sender_request_ = client_.find_search_senders(name);
        }
    };
    connect(find_sender, &QPushButton::clicked, this, lookup_sender);
    connect(sender_name, &QLineEdit::returnPressed, this, lookup_sender);
    connect(submit, &QPushButton::clicked, this, [this] { search(0); });
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
    connect(query_, &QLineEdit::textChanged, this, &global_search_dialog::filter_changed);
    connect(query_, &QLineEdit::returnPressed, this, [this] { search(0); });
    connect(&client_,
            &client_bridge::message_search_scopes_received,
            this,
            [this](quint64 request, std::vector<chat::message_search_scope> scopes, QString const& error)
            {
                if (request != scope_request_)
                {
                    return;
                }
                if (!error.isEmpty())
                {
                    if (!submitted_)
                    {
                        status_->setText(error);
                    }
                    return;
                }
                auto const selected = conversation_->currentData();
                QSignalBlocker const blocker(conversation_);
                while (conversation_->count() > 1)
                {
                    conversation_->removeItem(conversation_->count() - 1);
                }
                for (auto const& scope : scopes)
                {
                    conversation_->addItem(QString::fromStdString(scope.name) + QStringLiteral(" (#%1)").arg(scope.id),
                                           static_cast<qint64>(scope.id));
                }
                conversation_->setCurrentIndex(std::max(0, conversation_->findData(selected)));
                conversation_->setEnabled(true);
            });
    connect(&client_,
            &client_bridge::users_received,
            this,
            [this](QList<user_data> const& users, QString const& error, quint64 request)
            {
                if (request != sender_request_)
                {
                    return;
                }
                if (!error.isEmpty() || users.isEmpty())
                {
                    status_->setText(error.isEmpty() ? QStringLiteral("没有匹配的发送者") : error);
                    return;
                }
                for (auto const& user : users)
                {
                    if (sender_->findData(user.id) < 0)
                    {
                        sender_->addItem(user.username + QStringLiteral(" (#%1)").arg(user.id), user.id);
                    }
                }
                if (users.size() == 1)
                {
                    sender_->setCurrentIndex(sender_->findData(users.front().id));
                }
                else
                {
                    sender_->showPopup();
                }
            });
    connect(more_, &QPushButton::clicked, this, [this] { search(next_before_); });
    connect(back_, &QPushButton::clicked, this, &global_search_dialog::show_results);
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
            [this](qint64 id, QString const& action, qint64 through)
            {
                if (action == "clear")
                {
                    discard(id, through);
                }
            });
    connect(&client_, &client_bridge::disconnected, this, &QDialog::reject);
    connect(&client_,
            &client_bridge::conversation_changed,
            this,
            [this, self](qint64 conv, bool removed)
            {
                if (removed)
                {
                    remove_scope(conv);
                }
                if (self > 0)
                {
                    scope_request_ = client_.get_message_search_scopes();
                }
            });
    connect(&client_, &client_bridge::message_updated, this, &global_search_dialog::updated);
    if (self > 0)
    {
        scope_request_ = client_.get_message_search_scopes();
    }
    if (mentions_me)
    {
        search(0);
    }
}

void global_search_dialog::show_results()
{
    request_ = 0;
    context_conversation_ = context_anchor_ = 0;
    page_edits_.clear();
    page_floors_.clear();
    setWindowTitle(QStringLiteral("全局消息搜索"));
    context_->clear();
    context_->hide();
    back_->hide();
    query_->show();
    filters_->show();
    results_->show();
    more_->setVisible(next_before_ != 0);
    status_->clear();
}

void global_search_dialog::filter_changed()
{
    results_->clear();
    next_before_ = 0;
    submitted_ = false;
    mention_checks_.clear();
    show_results();
}

void global_search_dialog::search(qint64 before)
{
    if (!before)
    {
        filter_changed();
        auto id = [](QComboBox const* box) -> std::optional<std::int64_t>
        {
            auto const value = box->currentData().toLongLong();
            return value ? std::optional<std::int64_t>{value} : std::nullopt;
        };
        submitted_filter_ = {query_->text().toStdString(),
                             id(conversation_),
                             static_cast<chat::message_category>(category_->currentIndex()),
                             id(sender_),
                             {},
                             {},
                             mentions_me_->isChecked()};
        if (start_enabled_->isChecked())
        {
            submitted_filter_.start = start_date_->date().startOfDay().toMSecsSinceEpoch();
        }
        if (end_enabled_->isChecked())
        {
            submitted_filter_.end = end_date_->date().addDays(1).startOfDay().toMSecsSinceEpoch();
        }
        auto const& filter = submitted_filter_;
        if (filter.start && filter.end && *filter.start >= *filter.end)
        {
            status_->setText(QStringLiteral("结束日不能早于起始日"));
            return;
        }
        if (filter.query.empty() && !filter.conversation && filter.category == chat::message_category::all && !filter.sender && !filter.start &&
            !filter.end && !filter.mentions_me)
        {
            return;
        }
        submitted_ = true;
    }
    else
    {
        show_results();
    }
    status_->setText(QStringLiteral("正在搜索…"));
    more_->hide();
    request_ = client_.search_all_messages(submitted_filter_, before);
}

void global_search_dialog::open_context(QTreeWidgetItem* item)
{
    results_->setCurrentItem(item);
    show_context(item->data(0, Qt::UserRole).toLongLong(), item->data(1, Qt::UserRole).toLongLong(), item->text(1));
}

void global_search_dialog::locate(qint64 conversation, qint64 anchor, std::function<void()> unavailable)
{
    standalone_ = true;
    anchor_unavailable_ = std::move(unavailable);
    show_context(conversation, anchor, {});
}

void global_search_dialog::show_context(qint64 conversation, qint64 anchor, QString const& title)
{
    context_conversation_ = conversation;
    context_anchor_ = anchor;
    setWindowTitle(title.isEmpty() ? QStringLiteral("消息上下文") : QStringLiteral("聊天记录 · %1").arg(title));
    query_->hide();
    filters_->hide();
    results_->hide();
    more_->hide();
    context_->clear();
    context_->show();
    back_->setVisible(!standalone_);
    status_->setText(QStringLiteral("正在定位消息…"));
    request_ = client_.get_search_context(conversation, anchor);
}

void global_search_dialog::insert_hit(chat::message_search_hit const& hit, QString snippet)
{
    int row = 0;
    int end = results_->topLevelItemCount();
    while (row < end)
    {
        auto const middle = row + (end - row) / 2;
        auto const id = results_->topLevelItem(middle)->data(1, Qt::UserRole).toLongLong();
        if (id == hit.id)
        {
            return;
        }
        if (id > hit.id)
        {
            row = middle + 1;
        }
        else
        {
            end = middle;
        }
    }
    auto* item = new QTreeWidgetItem({snippet, QString::fromStdString(hit.title), QString::fromStdString(hit.username), display_time(hit.timestamp)});
    item->setData(0, Qt::UserRole, static_cast<qlonglong>(hit.conversation));
    item->setData(1, Qt::UserRole, static_cast<qlonglong>(hit.id));
    results_->insertTopLevelItem(row, item);
}

void global_search_dialog::set_page(quint64 id, chat::message_search_page const& page, QString const& error)
{
    if (auto check = mention_checks_.find(id); check != mention_checks_.end())
    {
        auto const target = check->second;
        mention_checks_.erase(check);
        if (error.isEmpty() && !page.messages.empty() && page.messages.front().id == target)
        {
            insert_hit(page.messages.front(), QString::fromStdString(page.messages.front().snippet));
            if (status_->text() == QStringLiteral("没有找到相关消息"))
            {
                status_->clear();
            }
        }
        return;
    }
    if (id != request_)
    {
        return;
    }
    request_ = 0;
    status_->setText(error);
    if (error.isEmpty())
    {
        for (auto const& hit : page.messages)
        {
            auto snippet = page_edits_.value(hit.id, QString::fromStdString(hit.snippet));
            if (hit.id <= page_floors_.value(hit.conversation) || snippet.isNull())
            {
                continue;
            }
            insert_hit(hit, std::move(snippet));
        }
        next_before_ = page.next_before.value_or(0);
        show_empty_result();
    }
    page_edits_.clear();
    page_floors_.clear();
    more_->setVisible(next_before_ != 0);
}
void global_search_dialog::set_context(quint64 id, qint64 anchor, QList<message_data> const& messages, QString const& error, bool unavailable)
{
    if (id != request_)
    {
        return;
    }
    request_ = 0;
    if (unavailable && standalone_)
    {
        context_unavailable();
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
        context_->item(context_->count() - 1)->setData(Qt::UserRole, m.id);
        if (m.id == anchor)
        {
            context_->setCurrentRow(context_->count() - 1);
        }
    }
    context_->scrollToItem(context_->currentItem(), QAbstractItemView::PositionAtCenter);
}
void global_search_dialog::remove_scope(qint64 conversation)
{
    if (auto index = conversation_->findData(conversation); index >= 0)
    {
        QSignalBlocker const blocker(conversation_);
        auto const selected = index == conversation_->currentIndex();
        conversation_->removeItem(index);
        if (selected)
        {
            conversation_->setCurrentIndex(0);
        }
    }
    // A rejoin can restore the dropdown, not the results of a request already in flight.
    discard(conversation, std::numeric_limits<qint64>::max());
    if (submitted_ && submitted_filter_.conversation == conversation && context_->isHidden())
    {
        request_ = 0;
        next_before_ = 0;
        more_->hide();
        status_->setText(QStringLiteral("原会话不可用"));
    }
}

void global_search_dialog::discard(qint64 conversation, qint64 through)
{
    mention_checks_.removeIf([=](decltype(mention_checks_)::iterator entry) { return entry->first == conversation && entry->second <= through; });
    if (!context_->isHidden() && context_conversation_ == conversation)
    {
        if (context_anchor_ <= through)
        {
            context_unavailable();
        }
        else if (request_ || (context_->count() && context_->item(0)->data(Qt::UserRole).toLongLong() <= through))
        {
            show_context(conversation, context_anchor_, standalone_ ? QString{} : results_->currentItem()->text(1));
        }
    }
    if (request_ && context_->isHidden())
    {
        page_floors_[conversation] = std::max(through, page_floors_.value(conversation));
    }
    auto const count = results_->topLevelItemCount();
    for (int i = count - 1; i >= 0; --i)
    {
        auto* item = results_->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toLongLong() == conversation && item->data(1, Qt::UserRole).toLongLong() <= through)
        {
            delete results_->takeTopLevelItem(i);
        }
    }
    if (results_->topLevelItemCount() != count)
    {
        show_empty_result();
    }
}

void global_search_dialog::context_unavailable()
{
    if (!standalone_)
    {
        show_results();
        return;
    }
    request_ = 0;
    context_conversation_ = context_anchor_ = 0;
    context_->clear();
    status_->setText(QStringLiteral("消息不可用"));
    if (anchor_unavailable_)
    {
        auto unavailable = std::move(anchor_unavailable_);
        unavailable();
    }
}

void global_search_dialog::show_empty_result()
{
    if (!results_->topLevelItemCount() && !request_ && !next_before_ && context_->isHidden() && status_->text().isEmpty() && submitted_)
    {
        status_->setText(QStringLiteral("没有找到相关消息"));
    }
}

void global_search_dialog::updated(qint64 conversation, message_data const& message, QString const& error)
{
    if (!error.isEmpty())
    {
        return;
    }
    auto* current = results_->currentItem();
    if (!context_->isHidden() && context_conversation_ == conversation)
    {
        auto included = request_ != 0;
        for (int i = 0; !included && i < context_->count(); ++i)
        {
            included = context_->item(i)->data(Qt::UserRole).toLongLong() == message.id;
        }
        if (message.id == context_anchor_ && message.deleted)
        {
            context_unavailable();
        }
        else if (included)
        {
            show_context(conversation, context_anchor_, standalone_ ? QString{} : current->text(1));
        }
    }
    if (!submitted_)
    {
        return;
    }
    mention_checks_.removeIf([&](decltype(mention_checks_)::iterator entry) { return entry->second == message.id; });
    auto snippet = [&]() -> QString
    {
        auto const& filter = submitted_filter_;
        if (message.deleted || (filter.conversation && *filter.conversation != conversation) || (filter.sender && *filter.sender != message.from) ||
            (filter.start && message.timestamp < *filter.start) || (filter.end && message.timestamp >= *filter.end))
        {
            return {};
        }
        if (filter.mentions_me && std::ranges::find(message.mentions, self_, &mention_data::user) == message.mentions.end())
        {
            return {};
        }
        using enum chat::message_category;
        auto const image = message.attachment && message.attachment->media_type.startsWith(QStringLiteral("image/"));
        if ((filter.category == images && !image) || (filter.category == files && (!message.attachment || image)) ||
            (filter.category == links && !message.text.contains(QStringLiteral("http://")) && !message.text.contains(QStringLiteral("https://"))))
        {
            return {};
        }
        return search_snippet(message.text, QString::fromStdString(filter.query));
    };
    for (int i = 0; i < results_->topLevelItemCount(); ++i)
    {
        auto* item = results_->topLevelItem(i);
        if (item->data(1, Qt::UserRole).toLongLong() != message.id)
        {
            continue;
        }
        auto text = snippet();
        if (text.isNull())
        {
            if (item == current && !context_->isHidden())
            {
                show_results();
            }
            delete results_->takeTopLevelItem(i);
            show_empty_result();
        }
        else
        {
            item->setText(0, text);
        }
        return;
    }
    auto text = snippet();
    if (request_ && context_->isHidden())
    {
        page_edits_.insert(message.id, text);
    }
    if (submitted_filter_.mentions_me && !text.isNull() && message.id < std::numeric_limits<qint64>::max())
    {
        auto filter = submitted_filter_;
        filter.conversation = conversation;
        mention_checks_.insert(client_.search_all_messages(filter, message.id + 1), {conversation, message.id});
    }
}
