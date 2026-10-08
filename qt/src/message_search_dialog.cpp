#include "message_search_dialog.hpp"
#include "avatar.hpp"
#include "theme.hpp"

#include <QClipboard>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPushButton>
#include <QItemSelectionModel>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QVBoxLayout>

#include "message_delegate.hpp"
#include "message_model.hpp"

class message_search_results_model final : public QSortFilterProxyModel
{
   public:
    explicit message_search_results_model(QObject* parent) : QSortFilterProxyModel(parent) {}
    void clear_hits() { hits_.clear(); invalidateFilter(); }
    void include_hits(QList<qint64> const& ids)
    {
        for (auto id : ids) { hits_.insert(id); }
        invalidateFilter();
    }
   protected:
    bool filterAcceptsRow(int row, QModelIndex const& parent) const override
    {
        auto const index = sourceModel()->index(row, 0, parent);
        return hits_.contains(index.data(message_model::id_role).toLongLong()) &&
               !index.data(message_model::deleted_role).toBool();
    }
   private:
    QSet<qint64> hits_;
};

message_search_dialog::message_search_dialog(qint64 conversation, qint64 self_user, bool group,
                                             QString const& title, QString const& query, QWidget* parent, avatar_cache* avatars)
    : QDialog(parent), conversation_(conversation), group_(group)
{
    setObjectName(QStringLiteral("messageSearchDialog"));
    setWindowTitle(title + QStringLiteral(" · 搜索消息"));
    resize(700, 600);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding,
                              chat_theme::dialog_padding, chat_theme::dialog_padding);
    layout->setSpacing(chat_theme::dialog_spacing);
    auto* search_row = new QHBoxLayout;
    search_row->setSpacing(chat_theme::dialog_spacing);
    input_ = new QLineEdit(this);
    input_->setObjectName(QStringLiteral("messageSearchEdit"));
    input_->setAccessibleName(QStringLiteral("消息搜索关键词"));
    input_->setPlaceholderText(QStringLiteral("输入当前会话的关键词"));
    input_->setMaxLength(256);
    search_button_ = new QPushButton(QStringLiteral("搜索"), this);
    search_button_->setObjectName(QStringLiteral("searchMessagesButton"));
    search_button_->setAutoDefault(false);
    search_button_->setDefault(true);
    search_row->addWidget(input_, 1);
    search_row->addWidget(search_button_);
    layout->addLayout(search_row);
    auto* help = new QLabel(QStringLiteral("不区分大小写。双击或按 Enter 跳转到聊天中的位置；消息被编辑后可重新搜索。"), this);
    help->setObjectName(QStringLiteral("messageSearchHelp"));
    help->setWordWrap(true);
    layout->addWidget(help);
    status_ = new QLabel(QStringLiteral("输入关键词后搜索。"), this);
    status_->setObjectName(QStringLiteral("messageSearchStatus"));
    status_->setWordWrap(true);
    layout->addWidget(status_);
    count_ = new QLabel(this);
    count_->setObjectName(QStringLiteral("messageSearchCount"));
    count_->setWordWrap(true);
    layout->addWidget(count_);
    messages_ = new message_model(this, avatars);
    messages_->set_self_user(self_user);
    messages_->reset(conversation_, group_);
    visible_messages_ = new message_search_results_model(this);
    visible_messages_->setSourceModel(messages_);
    results_ = new QListView(this);
    results_->setObjectName(QStringLiteral("messageSearchResults"));
    results_->setAccessibleName(QStringLiteral("消息搜索结果"));
    results_->setModel(visible_messages_);
    results_->setItemDelegate(new message_delegate(results_));
    results_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    results_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    results_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(results_, 1);
    more_button_ = new QPushButton(QStringLiteral("更早的结果"), this);
    more_button_->setObjectName(QStringLiteral("moreSearchResultsButton"));
    more_button_->setEnabled(false);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->addButton(more_button_, QDialogButtonBox::ActionRole);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    buttons->button(QDialogButtonBox::Close)->setIcon({});
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(search_button_, &QPushButton::clicked, this, [this] { request_search(false); });
    auto const activate = [this](QModelIndex const& index) {
        auto const id = index.data(message_model::id_role).toLongLong();
        if (id > 0) { emit message_activated(id); }
    };
    connect(results_, &QListView::activated, this, activate);
    connect(results_, &QListView::doubleClicked, this, activate);
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
        auto const text = index.data(message_model::text_role).toString();
        QMenu menu(this);
        auto* copy = menu.addAction(QStringLiteral("复制消息"));
        if (menu.exec(results_->viewport()->mapToGlobal(point)) == copy)
        {
            QGuiApplication::clipboard()->setText(text);
        }
    });
    refresh_count();
    if (!query.isEmpty())
    {
        input_->setText(query);
        QTimer::singleShot(0, this, [this] { request_search(false); });
    }
}

void message_search_dialog::request_search(bool older)
{
    if (!older)
    {
        query_ = input_->text().trimmed();
        if (query_.isEmpty())
        {
            status_->setText(QStringLiteral("请输入关键词。"));
            status_->show();
            refresh_count();
            return;
        }
        before_ = 0;
        next_before_ = 0;
        pending_reactions_.clear();
        messages_->reset(conversation_, group_);
        visible_messages_->clear_hits();
    }
    else
    {
        before_ = next_before_;
    }
    more_button_->setEnabled(false);
    status_->setText(QStringLiteral("正在搜索…"));
    status_->show();
    refresh_count();
    emit search_requested(query_, before_);
}

void message_search_dialog::set_reactions(qint64 conversation, qint64 message, qint64 revision,
                                         QList<reaction_data> reactions, QString const& error)
{
    if (conversation == conversation_ && message > 0 && error.isEmpty())
    {
        if (!messages_->set_reactions(message, revision, reactions))
        {
            auto const found = pending_reactions_.constFind(message);
            if (found == pending_reactions_.cend() || revision > found->revision)
            { pending_reactions_.insert(message, {revision, std::move(reactions)}); }
        }
    }
}

void message_search_dialog::update_message(qint64 conversation, message_data message, QString const& error)
{
    if (conversation != conversation_ || message.conversation != conversation_ || message.id <= 0 || !error.isEmpty()) { return; }
    auto const previous = selection();
    messages_->add_message(message);
    messages_->update_message(message);
    consume_reaction(message.id);
    restore_selection(previous);
    refresh_count();
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
        status_->show();
        more_button_->setEnabled(next_before_ > 0 && input_->text().trimmed() == query_);
        return;
    }
    auto const previous = selection();
    qint64 page_boundary = 0;
    QList<qint64> hits;
    for (auto const& message : messages)
    {
        if (message.conversation == conversation_ && message.id > 0)
        {
            hits.push_back(message.id);
            if (page_boundary == 0 || message.id < page_boundary) { page_boundary = message.id; }
        }
    }
    visible_messages_->include_hits(hits);
    messages_->merge_messages(std::move(messages));
    for (auto id : hits) { consume_reaction(id); }
    messages_->set_read_positions(std::move(positions));
    next_before_ = has_more ? page_boundary : 0;
    more_button_->setEnabled(next_before_ > 0 && input_->text().trimmed() == query_);
    status_->setText(before == 0 && hits.isEmpty() ? QStringLiteral("没有匹配的消息。") : QString{});
    status_->setVisible(!status_->text().isEmpty());
    restore_selection(previous);
    refresh_count();
    if (before == 0)
    {
        results_->scrollToBottom();
    }
}

message_search_dialog::selection_state message_search_dialog::selection() const
{
    selection_state state;
    auto const current = results_->currentIndex();
    state.current = current.data(message_model::id_role).toLongLong();
    state.row = current.row();
    auto const selected = results_->selectionModel()->selectedRows();
    if (!selected.isEmpty()) { state.selected = selected.front().data(message_model::id_role).toLongLong(); }
    return state;
}

void message_search_dialog::restore_selection(selection_state const& previous)
{
    auto* selection = results_->selectionModel();
    auto const count = visible_messages_->rowCount();
    if (count == 0) { selection->clear(); return; }
    if (previous.current == 0 && previous.selected == 0) { return; }
    auto find = [&](qint64 id) {
        for (int row = 0; row < count; ++row)
        {
            auto index = visible_messages_->index(row, 0);
            if (index.data(message_model::id_role).toLongLong() == id) { return index; }
        }
        return QModelIndex{};
    };
    auto current = find(previous.current);
    if (!current.isValid()) { current = visible_messages_->index(qBound(0, previous.row, count - 1), 0); }
    selection->clearSelection();
    selection->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
    if (previous.selected > 0)
    {
        auto selected = find(previous.selected);
        if (!selected.isValid()) { selected = current; }
        selection->select(selected, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
}

void message_search_dialog::consume_reaction(qint64 message)
{
    auto const found = pending_reactions_.constFind(message);
    if (found != pending_reactions_.cend() && messages_->set_reactions(message, found->revision, found->reactions))
    { pending_reactions_.remove(message); }
}

void message_search_dialog::refresh_count()
{
    auto const count = visible_messages_->rowCount();
    count_->setText(count > 0 ? QStringLiteral("已加载 %1 条搜索时命中的消息").arg(count)
                            : QStringLiteral("没有已加载的搜索命中；可重新搜索。"));
    count_->setVisible(count > 0 || status_->isHidden());
}
