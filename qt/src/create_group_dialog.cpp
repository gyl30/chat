#include "create_group_dialog.hpp"
#include "avatar.hpp"
#include "theme.hpp"
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
#include <QStyle>
#include <QShortcut>
#include <QVBoxLayout>
#include <algorithm>

create_group_dialog::create_group_dialog(QWidget* parent, bool invite) : QDialog(parent)
{
    setObjectName(invite ? QStringLiteral("groupInviteDialog") : QStringLiteral("createGroupDialog"));
    setWindowTitle(invite ? QStringLiteral("邀请好友") : QStringLiteral("创建群聊"));
    resize(425, 520);
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
    selected_->setWrapping(false);
    selected_->setMovement(QListView::Static);
    selected_->setIconSize({24, 24});
    selected_->setGridSize({100, 52});
    selected_->setFixedHeight(70);
    selected_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(selected_);

    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("groupContactPicker"));
    list_->setIconSize(QSize(32, 32));
    list_->setAccessibleName(QStringLiteral("选择群成员"));
    layout->addWidget(list_, 1);
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
        connect(title_, &QLineEdit::textChanged, this, &create_group_dialog::update_selection);
    }
    connect(list_,
            &QListWidget::itemClicked,
            this,
            [this](QListWidgetItem* item) { item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked); });
    auto* choose_by_keyboard = new QShortcut(QKeySequence(Qt::Key_Space), list_);
    choose_by_keyboard->setContext(Qt::WidgetShortcut);
    connect(choose_by_keyboard,
            &QShortcut::activated,
            list_,
            [this]
            {
                if (auto* item = list_->currentItem())
                {
                    item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
                }
            });
    connect(list_, &QListWidget::itemChanged, this, &create_group_dialog::update_selection);
    connect(selected_,
            &QListWidget::itemClicked,
            this,
            [this](QListWidgetItem* chip)
            {
                auto const id = chip->data(Qt::UserRole).toLongLong();
                for (int row = 0; row < list_->count(); ++row)
                {
                    auto* item = list_->item(row);
                    if (item->data(Qt::UserRole).toLongLong() == id)
                    {
                        item->setCheckState(Qt::Unchecked);
                        break;
                    }
                }
            });
    connect(search_,
            &QLineEdit::textChanged,
            this,
            [this](QString const& query)
            {
                for (int row = 0; row < list_->count(); ++row)
                {
                    list_->item(row)->setHidden(!list_->item(row)->data(user_model::search_role).toString().contains(query, Qt::CaseInsensitive));
                }
            });
    (title_ ? title_ : search_)->setFocus(Qt::OtherFocusReason);
}

void create_group_dialog::update_selection()
{
    QSignalBlocker blocked(selected_);
    selected_->clear();
    int count = 0;
    for (int row = 0; row < list_->count(); ++row)
    {
        auto* item = list_->item(row);
        if (item->checkState() != Qt::Checked)
        {
            continue;
        }
        auto* chip = new QListWidgetItem(item->icon(), item->text() + QStringLiteral(" ×"), selected_);
        chip->setData(Qt::UserRole, item->data(Qt::UserRole));
        chip->setToolTip(item->text());
        chip->setSizeHint(QSize(100, 52));
        ++count;
    }
    selected_count_->setText(QStringLiteral("已选 %1 人").arg(count));
    selected_count_->setVisible(count > 0);
    selected_->setVisible(count > 0);
    proceed_->setEnabled(count > 0 && (!title_ || chat::valid_group_title(title_->text().toUtf8().toStdString())));
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
            auto* item = new QListWidgetItem(
                avatar_icon(user.username, 32, avatars ? avatars->image(user.id) : QPixmap{}, user.id), user.display_name(), list_);
            item->setData(Qt::UserRole, user.id);
            item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
            item->setCheckState(chosen.contains(user.id) ? Qt::Checked : Qt::Unchecked);
            const auto search = QStringList{user.username, user.nickname, user.remark}.join(u'\n');
            item->setData(user_model::search_role, search);
            item->setToolTip(user.username);
            item->setSizeHint(QSize(0, 50));
            item->setHidden(!search.contains(search_->text(), Qt::CaseInsensitive));
        }
    }
    update_selection();
}

QString create_group_dialog::group_title() const { return title_->text(); }

QList<qint64> create_group_dialog::selected_members() const
{
    QList<qint64> members;
    for (int row = 0; row < list_->count(); ++row)
    {
        auto* item = list_->item(row);
        auto const id = item->data(Qt::UserRole).toLongLong();
        if (item->checkState() == Qt::Checked)
        {
            members.push_back(id);
        }
    }
    return members;
}
