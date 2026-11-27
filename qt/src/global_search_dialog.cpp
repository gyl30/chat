#include "theme.hpp"
#include "global_search_dialog.hpp"

#include <QDateTime>
#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QGridLayout>
#include <QInputDialog>
#include <QHBoxLayout>
#include <limits>
#include <algorithm>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QListWidget>
#include <QVBoxLayout>

#include <QSignalBlocker>
#include "client_bridge.hpp"
#include "chat_widget.hpp"
#include "message_model.hpp"
#include "message_delegate.hpp"
#include "message_preview.hpp"

namespace
{
bool has_criteria(chat::message_search_filter const& filter)
{
    return !filter.query.empty() || filter.conversation || filter.category != chat::message_category::all || filter.sender || filter.start ||
           filter.end || filter.mentions_me;
}
}    // namespace

global_search_dialog::global_search_dialog(
    client_bridge& client, chat_widget& chat, QWidget* parent, QString const& self_name, bool mentions_me, QString const& query, bool submit_query)
    : QDialog(parent), client_(client), chat_(chat), self_(chat.self_user())
{
    setWindowTitle(QStringLiteral("全局消息搜索"));
    resize(760, 540);
    auto* layout = new QVBoxLayout(this);
    frame_dialog(*this);
    query_ = new QLineEdit(this);
    style_search_field(*query_);
    query_->setPlaceholderText(QStringLiteral("搜索聊天记录"));
    query_->setObjectName(QStringLiteral("globalSearchQuery"));
    query_->setText(query);
    header_ = new QWidget(this);
    auto* header = new QHBoxLayout(header_);
    header->setContentsMargins(0, 0, 0, 0);
    header->addWidget(query_, 1);
    submit_ = new QPushButton(QStringLiteral("搜索"), header_);
    submit_->setObjectName(QStringLiteral("submitGlobalSearch"));
    submit_->setProperty("primary", true);
    header->addWidget(submit_);
    toggle_filters_ = new QPushButton(QStringLiteral("筛选条件"), header_);
    toggle_filters_->setObjectName(QStringLiteral("toggleSearchFilters"));
    toggle_filters_->setCheckable(true);
    toggle_filters_->setChecked(mentions_me);
    header->addWidget(toggle_filters_);
    layout->addWidget(header_);
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
    if (!self_name.isEmpty())
    {
        sender_->addItem(QStringLiteral("%1（我）").arg(self_name), self_);
    }
    sender_->addItem(QStringLiteral("查找其他发送者…"), qint64(-1));
    connect(sender_,
            &QComboBox::activated,
            this,
            [this](int index)
            {
                if (sender_->itemData(index).toLongLong() != -1)
                {
                    return;
                }
                sender_->setCurrentIndex(0);
                bool ok = false;
                auto text = prompt_text(this, QStringLiteral("查找发送者"), QStringLiteral("用户名或昵称"), {}, ok).trimmed();
                if (ok && !text.isEmpty())
                {
                    sender_request_ = client_.find_search_senders(text);
                }
            });
    submit_->setAutoDefault(false);
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
    period_ = new QComboBox(filters_);
    period_->setObjectName(QStringLiteral("searchPeriod"));
    period_->addItems({QStringLiteral("全部时间"), QStringLiteral("最近 7 天"), QStringLiteral("最近 30 天"), QStringLiteral("自定义日期")});
    fields->addWidget(period_, 1, 0);
    fields->addWidget(mentions_me_, 1, 1);
    auto* reset = new QPushButton(QStringLiteral("重置筛选"), filters_);
    reset->setAutoDefault(false);
    fields->addWidget(reset, 1, 2);
    auto* dates = new QWidget(filters_);
    auto* dates_layout = new QHBoxLayout(dates);
    dates_layout->setContentsMargins(0, 0, 0, 0);
    for (auto* w : {static_cast<QWidget*>(start_enabled_),
                    static_cast<QWidget*>(start_date_),
                    static_cast<QWidget*>(end_enabled_),
                    static_cast<QWidget*>(end_date_)})
    {
        dates_layout->addWidget(w);
    }
    fields->addWidget(dates, 2, 0, 1, 3);
    dates->hide();
    connect(period_,
            &QComboBox::currentIndexChanged,
            this,
            [this, dates](int index)
            {
                dates->setVisible(index == 3);
                filter_changed();
            });
    connect(toggle_filters_, &QPushButton::toggled, filters_, &QWidget::setVisible);
    connect(reset,
            &QPushButton::clicked,
            this,
            [this]
            {
                sender_request_ = 0;
                for (auto* box : {conversation_, category_, sender_, period_})
                {
                    box->setCurrentIndex(0);
                }
                start_enabled_->setChecked(false);
                end_enabled_->setChecked(false);
                mentions_me_->setChecked(false);
                start_date_->setDate(QDate::currentDate());
                end_date_->setDate(QDate::currentDate());
            });
    layout->addWidget(filters_);
    for (auto* box : {conversation_, category_, sender_})
    {
        connect(box, &QComboBox::currentIndexChanged, this, &global_search_dialog::filter_changed);
    }
    connect(submit_, &QPushButton::clicked, this, [this] { search(0); });
    back_ = new QPushButton(QStringLiteral("返回搜索结果"), this);
    back_->setAutoDefault(false);
    layout->addWidget(back_, 0, Qt::AlignLeft);
    results_ = new QListWidget(this);
    results_->setObjectName(QStringLiteral("globalSearchResults"));
    results_->setAccessibleName(QStringLiteral("消息搜索结果"));
    style_message_previews(*results_, *query_, &chat.avatars());
    layout->addWidget(results_);
    context_ = new QListView(this);
    context_->setObjectName(QStringLiteral("searchContextMessages"));
    context_->setFrameShape(QFrame::NoFrame);
    context_->setAccessibleName(QStringLiteral("消息上下文"));
    context_->setSelectionMode(QAbstractItemView::NoSelection);
    context_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    context_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    context_->setResizeMode(QListView::Adjust);
    messages_ = new message_model(context_, &chat.avatars());
    messages_->set_self_user(self_);
    context_->setModel(messages_);
    context_->setItemDelegate(new message_delegate(context_, &context_anchor_, &chat.avatars()));
    layout->addWidget(context_, 1);
    status_ = new QLabel(this);
    status_->setTextFormat(Qt::PlainText);
    status_->setObjectName(QStringLiteral("globalSearchStatus"));
    status_->setAlignment(Qt::AlignCenter);
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
                        set_status(error);
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
                if (!sender_request_ || request != sender_request_)
                {
                    return;
                }
                if (!error.isEmpty() || users.isEmpty())
                {
                    set_status(error.isEmpty() ? QStringLiteral("没有匹配的发送者") : error);
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
    connect(results_, &QListWidget::itemClicked, this, &global_search_dialog::open_context);
    connect(results_,
            &QListWidget::itemActivated,
            this,
            [this](QListWidgetItem* item)
            {
                if (!results_->isHidden())
                {
                    open_context(item);
                }
            });
    connect(&client_, &client_bridge::global_search_received, this, &global_search_dialog::set_page);
    connect(&client_, &client_bridge::search_context_received, this, &global_search_dialog::set_context);
    connect(&client_,
            &client_bridge::members_received,
            this,
            [this](qint64 conversation, QList<member_data> const& members, QString const& error)
            {
                if (conversation == context_conversation_ && error.isEmpty())
                {
                    for (auto const& member : members)
                    {
                        chat_.avatars().observe(member.id, member.avatar);
                    }
                    messages_->set_members(members);
                }
            });
    connect(&client_,
            &client_bridge::messages_read,
            this,
            [this](qint64 conversation, qint64 user, qint64 message)
            {
                if (conversation == context_conversation_)
                {
                    messages_->set_read_message(user, message);
                }
            });
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
            [this](qint64 conv, bool removed)
            {
                if (removed)
                {
                    remove_scope(conv);
                }
                else if (conv == context_conversation_)
                {
                    // The main window is already fetching the replacement roster.
                    messages_->set_members({});
                }
                if (!standalone_)
                {
                    scope_request_ = client_.get_message_search_scopes();
                }
            });
    connect(&client_, &client_bridge::message_updated, this, &global_search_dialog::updated);
    connect(&client_,
            &client_bridge::poll_changed,
            this,
            [this](chat::poll_update const& update)
            {
                if (update.conversation == context_conversation_)
                {
                    messages_->set_poll(update.message, update.summary);
                }
            });
    if (!self_name.isEmpty())
    {
        scope_request_ = client_.get_message_search_scopes();
    }
    query_->setFocus(Qt::OtherFocusReason);
    if (mentions_me || submit_query)
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
    messages_->reset(0);
    context_->hide();
    back_->hide();
    header_->show();
    submit_->setEnabled(has_criteria(search_filter()));
    filters_->setVisible(toggle_filters_->isChecked());
    results_->show();
    more_->setVisible(next_before_ != 0);
    set_status();
}

void global_search_dialog::filter_changed()
{
    results_->clear();
    next_before_ = 0;
    submitted_ = false;
    mention_checks_.clear();
    show_results();
}

void global_search_dialog::set_status(QString message)
{
    status_->setText(std::move(message));
    status_->setVisible(!status_->text().isEmpty());
}

chat::message_search_filter global_search_dialog::search_filter() const
{
    auto id = [](QComboBox const* box) -> std::optional<std::int64_t>
    {
        auto const value = box->currentData().toLongLong();
        return value > 0 ? std::optional<std::int64_t>{value} : std::nullopt;
    };
    auto const query = query_->text();
    chat::message_search_filter filter{query.trimmed().isEmpty() ? std::string{} : query.toStdString(),
                                      id(conversation_),
                                      static_cast<chat::message_category>(category_->currentIndex()),
                                      id(sender_),
                                      {},
                                      {},
                                      mentions_me_->isChecked()};
    auto const period = period_->currentIndex();
    if (period == 1 || period == 2)
    {
        auto const today = QDate::currentDate();
        filter.start = today.addDays(period == 1 ? -6 : -29).startOfDay().toMSecsSinceEpoch();
        filter.end = today.addDays(1).startOfDay().toMSecsSinceEpoch();
    }
    if (period == 3 && start_enabled_->isChecked())
    {
        filter.start = start_date_->date().startOfDay().toMSecsSinceEpoch();
    }
    if (period == 3 && end_enabled_->isChecked())
    {
        filter.end = end_date_->date().addDays(1).startOfDay().toMSecsSinceEpoch();
    }
    return filter;
}

void global_search_dialog::search(qint64 before)
{
    // A visibility change invalidates the snapshot even when the search text is unchanged.
    if (request_ && (before || page_floors_.isEmpty()))
    {
        return;
    }
    if (!before)
    {
        filter_changed();
        submitted_filter_ = search_filter();
        auto const& filter = submitted_filter_;
        if (filter.start && filter.end && *filter.start >= *filter.end)
        {
            set_status(QStringLiteral("结束日不能早于起始日"));
            return;
        }
        if (!has_criteria(filter))
        {
            return;
        }
        submitted_ = true;
    }
    else
    {
        show_results();
    }
    set_status(QStringLiteral("正在搜索…"));
    more_->hide();
    request_ = client_.search_all_messages(submitted_filter_, before);
}

void global_search_dialog::open_context(QListWidgetItem* item)
{
    results_->setCurrentItem(item);
    show_context(item->data(preview_role::conversation).toLongLong(), item->data(preview_role::message).toLongLong(), item->data(preview_role::title).toString());
}

void global_search_dialog::locate(qint64 conversation, qint64 anchor, std::function<void()> unavailable, QString const& title)
{
    standalone_ = true;
    anchor_unavailable_ = std::move(unavailable);
    show_context(conversation, anchor, title);
}

void global_search_dialog::show_context(qint64 conversation, qint64 anchor, QString const& title)
{
    context_conversation_ = conversation;
    context_anchor_ = anchor;
    setWindowTitle(title.isEmpty() ? QStringLiteral("消息上下文") : QStringLiteral("聊天记录 · %1").arg(title));
    header_->hide();
    filters_->hide();
    results_->hide();
    more_->hide();
    auto const room = chat_.conversation(conversation);
    // Hidden rooms are in the sidebar model too. Until a new room is known,
    // require its roster instead of treating any reader as the peer of a direct chat.
    const auto group = !room || room->group;
    messages_->reset(conversation, group);
    if (group)
    {
        client_.get_members(conversation);
    }
    messages_->begin_page();
    context_->show();
    back_->setVisible(!standalone_);
    set_status(QStringLiteral("正在定位消息…"));
    request_ = client_.get_search_context(conversation, anchor);
}

void global_search_dialog::insert_hit(chat::message_search_hit const& hit, QString snippet)
{
    int row = 0;
    int end = results_->count();
    while (row < end)
    {
        auto const middle = row + (end - row) / 2;
        auto const id = results_->item(middle)->data(preview_role::message).toLongLong();
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
    auto* item = new QListWidgetItem;
    set_message_preview(*item, hit, std::move(snippet));
    results_->insertItem(row, item);
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
                set_status();
            }
        }
        return;
    }
    if (id != request_)
    {
        return;
    }
    request_ = 0;
    set_status(error);
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
void global_search_dialog::set_context(
    quint64 id, qint64 anchor, QList<message_data> const& messages, read_positions const& positions, QString const& error, bool unavailable)
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
        messages_->end_page();
        set_status(error);
        return;
    }
    set_status();
    messages_->merge_messages(messages);
    messages_->set_read_positions(positions);
    auto const hits = messages_->match(messages_->index(0), message_model::id_role, anchor, 1, Qt::MatchExactly);
    if (!hits.isEmpty())
    {
        context_->setCurrentIndex(hits.front());
        context_->scrollTo(hits.front(), QAbstractItemView::PositionAtCenter);
    }
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
        set_status(QStringLiteral("原会话不可用"));
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
        else
        {
            messages_->discard_through(through);
            if (request_)
            {
                request_ = client_.get_search_context(conversation, context_anchor_);
            }
        }
    }
    if (request_ && context_->isHidden())
    {
        page_floors_[conversation] = std::max(through, page_floors_.value(conversation));
    }
    auto const count = results_->count();
    for (int i = count - 1; i >= 0; --i)
    {
        auto* item = results_->item(i);
        if (item->data(preview_role::conversation).toLongLong() == conversation && item->data(preview_role::message).toLongLong() <= through)
        {
            delete results_->takeItem(i);
        }
    }
    if (results_->count() != count)
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
    messages_->reset(0);
    set_status(QStringLiteral("消息不可用"));
    if (anchor_unavailable_)
    {
        auto unavailable = std::move(anchor_unavailable_);
        unavailable();
    }
}

void global_search_dialog::show_empty_result()
{
    if (!results_->count() && !request_ && !next_before_ && context_->isHidden() && status_->text().isEmpty() && submitted_)
    {
        set_status(QStringLiteral("没有找到相关消息"));
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
        if (message.id == context_anchor_ && message.deleted)
        {
            context_unavailable();
        }
        else if (request_)
        {
            // The outstanding snapshot may predate this edit, including in its quoted replies.
            request_ = client_.get_search_context(conversation, context_anchor_);
        }
        else
        {
            messages_->update_message(message);
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
        if (filter.mentions_me && !(message.mention_all && message.from != self_) &&
            std::ranges::find(message.mentions, self_, &mention_data::user) == message.mentions.end())
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
        return matched_message_excerpt(message.text, QString::fromStdString(filter.query));
    };
    for (int i = 0; i < results_->count(); ++i)
    {
        auto* item = results_->item(i);
        if (item->data(preview_role::message).toLongLong() != message.id)
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
            delete results_->takeItem(i);
            show_empty_result();
        }
        else
        {
            set_preview_snippet(*item, std::move(text));
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
