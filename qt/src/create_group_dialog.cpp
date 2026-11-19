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
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>
#include <algorithm>

create_group_dialog::create_group_dialog(QWidget* parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("createGroupDialog"));
    setWindowTitle(QStringLiteral("创建群聊"));
    resize(chat_theme::dialog_normal_width, 540);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding);
    layout->setSpacing(chat_theme::dialog_spacing);
    steps_ = new QStackedWidget(this);
    setup_contact_picker();
    setup_naming_page();
    layout->addWidget(steps_, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* previous = buttons->addButton(QStringLiteral("上一步"), QDialogButtonBox::ActionRole);
    previous->setObjectName(QStringLiteral("groupPreviousButton"));
    previous->hide();
    proceed_ = buttons->button(QDialogButtonBox::Ok);
    proceed_->setObjectName(QStringLiteral("groupNextButton"));
    proceed_->setText(QStringLiteral("下一步"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    for (auto* button : buttons->buttons())
    {
        button->setIcon({});
        button->style()->unpolish(button);
        button->style()->polish(button);
    }
    layout->addWidget(buttons);

    connect(title_, &QLineEdit::textChanged, this, &create_group_dialog::update_selection);
    connect(previous,
            &QPushButton::clicked,
            this,
            [this, previous]
            {
                steps_->setCurrentIndex(0);
                previous->hide();
                proceed_->setText(QStringLiteral("下一步"));
                resize(chat_theme::dialog_normal_width, 540);
                search_->setFocus();
                update_selection();
            });
    connect(buttons,
            &QDialogButtonBox::accepted,
            this,
            [this, previous]
            {
                if (steps_->currentIndex() == 0)
                {
                    steps_->setCurrentIndex(1);
                    previous->show();
                    proceed_->setText(QStringLiteral("创建"));
                    update_selection();
                    resize(chat_theme::dialog_normal_width, 300);
                    title_->setFocus();
                }
                else if (chat::valid_group_title(title_->text().toUtf8().toStdString()))
                {
                    accept();
                }
            });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void create_group_dialog::setup_contact_picker()
{
    auto* pick_page = new QWidget(steps_);
    pick_page->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    auto* pick_layout = new QVBoxLayout(pick_page);
    pick_layout->setContentsMargins(0, 0, 0, 0);
    pick_layout->setSpacing(chat_theme::dialog_spacing);
    search_ = new QLineEdit(pick_page);
    search_->setObjectName(QStringLiteral("groupContactSearch"));
    search_->setPlaceholderText(QStringLiteral("搜索好友"));
    pick_layout->addWidget(search_);
    selected_count_ = new QLabel(pick_page);
    selected_count_->setObjectName(QStringLiteral("groupSelectedCount"));
    pick_layout->addWidget(selected_count_);
    selected_ = new QListWidget(pick_page);
    selected_->setObjectName(QStringLiteral("groupSelectedContacts"));
    selected_->setTextElideMode(Qt::ElideRight);
    selected_->setFlow(QListView::LeftToRight);
    selected_->setWrapping(true);
    selected_->setResizeMode(QListView::Adjust);
    pick_layout->addWidget(selected_);
    list_ = new QListWidget(pick_page);
    list_->setObjectName(QStringLiteral("groupContactPicker"));
    list_->setIconSize(QSize(32, 32));
    list_->setAccessibleName(QStringLiteral("选择群成员"));
    pick_layout->addWidget(list_, 1);
    steps_->addWidget(pick_page);
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
                    list_->item(row)->setHidden(!list_->item(row)->text().contains(query, Qt::CaseInsensitive));
                }
            });
}

void create_group_dialog::setup_naming_page()
{
    auto* name_page = new QWidget(steps_);
    auto* name_layout = new QVBoxLayout(name_page);
    name_layout->setContentsMargins(0, 0, 0, 0);
    name_layout->setSpacing(chat_theme::dialog_spacing);
    name_layout->addWidget(new QLabel(QStringLiteral("群名称"), name_page));
    title_ = new QLineEdit(name_page);
    title_->setObjectName(QStringLiteral("newGroupTitleEdit"));
    title_->setPlaceholderText(QStringLiteral("群名称"));
    title_->setAccessibleName(QStringLiteral("群名称"));
    name_layout->addWidget(title_);
    summary_ = new QLabel(name_page);
    summary_->setObjectName(QStringLiteral("groupNamingSummary"));
    name_layout->addWidget(summary_);
    member_list_ = new QListWidget(name_page);
    member_list_->setObjectName(QStringLiteral("groupNamingMembers"));
    member_list_->setIconSize(QSize(32, 32));
    member_list_->setSelectionMode(QAbstractItemView::NoSelection);
    member_list_->setAccessibleName(QStringLiteral("已选群成员"));
    name_layout->addWidget(member_list_, 1);
    steps_->addWidget(name_page);
}

void create_group_dialog::update_selection()
{
    QSignalBlocker blocked(selected_);
    selected_->clear();
    member_list_->clear();
    int count = 0;
    for (int row = 0; row < list_->count(); ++row)
    {
        auto* item = list_->item(row);
        if (item->checkState() != Qt::Checked)
        {
            continue;
        }
        auto* chip = new QListWidgetItem(item->text() + QStringLiteral(" ×"), selected_);
        chip->setData(Qt::UserRole, item->data(Qt::UserRole));
        chip->setToolTip(item->text());
        chip->setSizeHint(QSize(210, 32));
        auto* member = new QListWidgetItem(item->icon(), item->text(), member_list_);
        member->setToolTip(item->text());
        member->setSizeHint(QSize(0, 44));
        ++count;
    }
    selected_count_->setText(QStringLiteral("已选 %1 位好友 · 点击姓名取消").arg(count));
    selected_->setVisible(count > 0);
    selected_->setFixedHeight(count <= 2 ? 40 : 76);
    summary_->setText(QStringLiteral("已选 %1 位好友").arg(count));
    member_list_->setFixedHeight(std::clamp(count * 44 + 4, 48, 136));
    proceed_->setEnabled(count > 0 && (steps_->currentIndex() == 0 || chat::valid_group_title(title_->text().toUtf8().toStdString())));
}

void create_group_dialog::set_contacts(user_model const& contacts, qint64 self, avatar_cache const& avatars)
{
    auto const ids = selected_members();
    QSet<qint64> const chosen(ids.begin(), ids.end());
    {
        QSignalBlocker blocked(list_);
        list_->clear();
        for (int row = 0; row < contacts.rowCount(); ++row)
        {
            auto const* user = contacts.user_at(contacts.index(row, 0));
            if (!user || user->id == self)
            {
                continue;
            }
            auto* item = new QListWidgetItem(avatar_icon(user->username, 32, avatars.image(user->id)), user->username, list_);
            item->setData(Qt::UserRole, user->id);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(chosen.contains(user->id) ? Qt::Checked : Qt::Unchecked);
            item->setToolTip(user->username);
            item->setSizeHint(QSize(0, 50));
            item->setHidden(!user->username.contains(search_->text(), Qt::CaseInsensitive));
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
