#include "empty_state.hpp"
#include "create_group_dialog.hpp"
#include "avatar.hpp"
#include "theme.hpp"
#include "theme_manager.hpp"
#include "user_delegate.hpp"
#include "emoji_text.hpp"
#include "user_model.hpp"
#include <chat/text.hpp>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QHBoxLayout>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollBar>
#include <QShortcut>
#include <QVBoxLayout>
#include <algorithm>

namespace
{
constexpr QSize chip_size{136, 38};

class selected_member_delegate final : public QStyledItemDelegate
{
   public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(QStyleOptionViewItem const&, QModelIndex const&) const override { return chip_size; }

    void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override
    {
        auto const rect = option.rect.adjusted(2, 2, -4, -2);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        bool const focused = option.state.testFlag(QStyle::State_HasFocus);
        painter->setPen(focused ? QPen(themed("focus")) : Qt::NoPen);
        painter->setBrush(themed(option.state.testFlag(QStyle::State_MouseOver) ? "selected" : "subtle"));
        painter->drawRoundedRect(rect, 15, 15);
        auto const name = index.data().toString();
        paint_avatar(*painter, {rect.left() + 4, rect.center().y() - 11, 24, 24}, name, 11, index.data(Qt::DecorationRole).value<QPixmap>());
        paint_emoji_line(*painter, rect.adjusted(34, 0, -24, 0), name, option.font, themed("text"));
        QPoint const close{rect.right() - 12, rect.center().y()};
        painter->setPen(QPen(themed("text_secondary"), 1.2, Qt::SolidLine, Qt::RoundCap));
        painter->drawLine(close + QPoint(-3, -3), close + QPoint(3, 3));
        painter->drawLine(close + QPoint(-3, 3), close + QPoint(3, -3));
        painter->restore();
    }
};

void toggle_member(QListWidgetItem& item)
{
    bool const checked = !item.data(user_delegate::chosen_role).toBool();
    item.setData(Qt::AccessibleDescriptionRole, checked ? QStringLiteral("已选中") : QStringLiteral("未选中"));
    item.setData(user_delegate::chosen_role, checked);
}
}    // namespace

create_group_dialog::create_group_dialog(QWidget* parent, bool invite) : QDialog(parent)
{
    setObjectName(invite ? QStringLiteral("groupInviteDialog") : QStringLiteral("createGroupDialog"));
    setWindowTitle(invite ? QStringLiteral("邀请好友") : QStringLiteral("创建群聊"));
    auto* layout = new QVBoxLayout(this);
    frame_dialog(*this);
    layout->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding);
    layout->setSpacing(chat_theme::dialog_spacing);
    title_ = invite ? nullptr : new QLineEdit(this);
    if (title_)
    {
        title_->setObjectName(QStringLiteral("newGroupTitleEdit"));
        title_->setPlaceholderText(QStringLiteral("群名称"));
        title_->setAccessibleName(title_->placeholderText());
        layout->addWidget(title_);
    }
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("groupContactSearch"));
    style_search_field(*search_);
    search_->setPlaceholderText(QStringLiteral("搜索好友"));
    layout->addWidget(search_);

    selected_count_ = new QLabel(this);
    layout->addWidget(selected_count_);
    selected_ = new QListWidget(this);
    selected_->setObjectName(QStringLiteral("groupSelectedContacts"));
    selected_->setTextElideMode(Qt::ElideRight);
    selected_->setAccessibleName(QStringLiteral("已选成员，点击移除"));
    selected_->setViewMode(QListView::IconMode);
    selected_->setFlow(QListView::LeftToRight);
    selected_->setWrapping(true);
    selected_->setResizeMode(QListView::Adjust);
    selected_->setMovement(QListView::Static);
    selected_->setGridSize(chip_size);
    selected_->setItemDelegate(new selected_member_delegate(selected_));
    selected_->setMouseTracking(true);
    selected_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    selected_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(selected_);

    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("groupContactPicker"));
    list_->setAccessibleName(QStringLiteral("选择群成员"));
    list_->setItemDelegate(new user_delegate(list_, {}, user_delegate::layout::picker));
    list_->setMouseTracking(true);
    list_->setUniformItemSizes(true);
    list_->setMinimumHeight(110);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(list_, 1);
    empty_ = new empty_state(*list_->viewport(), QStringLiteral("groupPickerEmpty"));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    proceed_ = buttons->button(QDialogButtonBox::Ok);
    proceed_->setObjectName(invite ? QStringLiteral("groupInviteSubmitButton") : QStringLiteral("groupCreateButton"));
    proceed_->setProperty("primary", true);
    proceed_->setText(invite ? QStringLiteral("邀请") : QStringLiteral("创建群聊"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    if (title_)
    {
        connect(title_, &QLineEdit::textChanged, this, &create_group_dialog::update_proceed);
    }
    connect(list_, &QListWidget::itemClicked, this, [](QListWidgetItem* item) { toggle_member(*item); });
    auto* choose_by_keyboard = new QShortcut(QKeySequence(Qt::Key_Space), list_);
    choose_by_keyboard->setContext(Qt::WidgetShortcut);
    connect(choose_by_keyboard,
            &QShortcut::activated,
            list_,
            [this]
            {
                if (auto* item = list_->currentItem())
                {
                    toggle_member(*item);
                }
            });
    connect(list_->model(),
            &QAbstractItemModel::dataChanged,
            this,
            [this](QModelIndex const&, QModelIndex const&, QList<int> const& roles)
            {
                if (roles.contains(user_delegate::chosen_role))
                {
                    update_selection();
                }
            });
    auto remove = [this](QListWidgetItem* chip)
    {
        auto const id = chip->data(Qt::UserRole).toLongLong();
        for (int row = 0; row < list_->count(); ++row)
        {
            auto* item = list_->item(row);
            if (item->data(Qt::UserRole).toLongLong() == id)
            {
                toggle_member(*item);
                break;
            }
        }
    };
    connect(selected_, &QListWidget::itemClicked, this, remove);
    connect(selected_, &QListWidget::itemActivated, this, remove);
    connect(search_, &QLineEdit::textChanged, this, &create_group_dialog::filter_contacts);
    update_selection();
    (title_ ? title_ : search_)->setFocus(Qt::OtherFocusReason);
}

void create_group_dialog::filter_contacts()
{
    auto const query = search_->text().trimmed();
    bool found = false;
    for (int row = 0; row < list_->count(); ++row)
    {
        auto* item = list_->item(row);
        const bool match = item->data(user_model::search_role).toString().contains(query, Qt::CaseInsensitive);
        item->setHidden(!match);
        found |= match;
    }
    empty_->set_message(found             ? QString{}
                        : query.isEmpty() ? QStringLiteral("暂无可选好友")
                                          : QStringLiteral("未找到好友"),
                        query.isEmpty() ? u"contacts" : u"search");
}

void create_group_dialog::update_selection()
{
    QSignalBlocker blocked(selected_);
    auto const scroll = selected_->verticalScrollBar()->value();
    selected_->clear();
    int count = 0;
    for (int row = 0; row < list_->count(); ++row)
    {
        auto* item = list_->item(row);
        if (!item->data(user_delegate::chosen_role).toBool())
        {
            continue;
        }
        auto* chip = new QListWidgetItem(item->text(), selected_);
        chip->setData(Qt::DecorationRole, item->data(Qt::DecorationRole));
        chip->setData(Qt::AccessibleDescriptionRole, QStringLiteral("激活以移除"));
        chip->setData(Qt::UserRole, item->data(Qt::UserRole));
        chip->setToolTip(item->text());
        ++count;
    }
    selected_count_->setText(QStringLiteral("已选 %1 人").arg(count));
    selected_count_->setVisible(count > 0);
    selected_->setVisible(count > 0);
    update_chip_height();
    selected_->verticalScrollBar()->setValue(scroll);
    update_proceed();
}

void create_group_dialog::update_proceed()
{
    proceed_->setEnabled(selected_->count() > 0 && (!title_ || chat::valid_group_title(title_->text().toUtf8().toStdString())));
}

void create_group_dialog::update_chip_height()
{
    if (selected_->count())
    {
        auto const columns = std::max(1, selected_->width() / chip_size.width());
        auto const rows = (selected_->count() - 1) / columns + 1;
        selected_->setFixedHeight(std::min(rows, 3) * chip_size.height() + 2);
    }
}

void create_group_dialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    update_chip_height();
}

void create_group_dialog::set_contacts(user_model const& contacts, qint64 self, avatar_cache const& avatars)
{
    QList<user_data> candidates;
    for (int row = 0; row < contacts.rowCount(); ++row)
    {
        if (auto const* user = contacts.user_at(contacts.index(row, 0)); user && user->id != self)
        {
            candidates.push_back(*user);
        }
    }
    set_candidates(candidates, &avatars);
}

void create_group_dialog::set_candidates(QList<user_data> const& candidates, avatar_cache const* avatars)
{
    const auto ids = selected_members();
    const QSet<qint64> chosen(ids.begin(), ids.end());
    {
        const QSignalBlocker blocked(list_);
        list_->clear();
        for (auto const& user : candidates)
        {
            auto* item = new QListWidgetItem(user.display_name(), list_);
            item->setData(Qt::UserRole, user.id);
            item->setData(Qt::DecorationRole, avatars ? avatars->image(user.id) : QPixmap{});
            item->setData(user_model::online_role, user.online);
            item->setData(user_model::last_seen_role, user.last_seen);
            item->setData(user_delegate::chosen_role, chosen.contains(user.id));
            item->setData(Qt::AccessibleDescriptionRole, chosen.contains(user.id) ? QStringLiteral("已选中") : QStringLiteral("未选中"));
            const auto search = QStringList{user.username, user.nickname, user.remark}.join(u'\n');
            item->setData(user_model::search_role, search);
            item->setToolTip(user.username);
        }
    }
    filter_contacts();
    update_selection();
    if (!sized_)
    {
        sized_ = true;
        layout()->activate();
        auto const viewport = std::clamp<int>(list_->count() * chat_theme::member_row_height + 4, list_->minimumHeight(), 380);
        auto const requested = layout()->minimumSize().height() - list_->minimumHeight() + viewport;
        resize(452, std::min(requested, screen()->availableGeometry().height() - 46));
    }
}

QString create_group_dialog::group_title() const { return title_->text(); }

QList<qint64> create_group_dialog::selected_members() const
{
    QList<qint64> members;
    for (int row = 0; row < list_->count(); ++row)
    {
        auto* item = list_->item(row);
        auto const id = item->data(Qt::UserRole).toLongLong();
        if (item->data(user_delegate::chosen_role).toBool())
        {
            members.push_back(id);
        }
    }
    return members;
}
