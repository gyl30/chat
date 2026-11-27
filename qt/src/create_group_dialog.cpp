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
#include <QVBoxLayout>
#include <algorithm>

create_group_dialog::create_group_dialog(QWidget* parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("createGroupDialog"));
    setWindowTitle(QStringLiteral("创建群聊"));
    resize(640, 500);
    auto* layout = new QVBoxLayout(this);
    frame_dialog(*this);
    layout->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding);
    layout->setSpacing(chat_theme::dialog_spacing);
    layout->addWidget(new QLabel(QStringLiteral("群名称"), this));
    title_ = new QLineEdit(this);
    title_->setObjectName(QStringLiteral("newGroupTitleEdit"));
    title_->setPlaceholderText(QStringLiteral("输入群名称"));
    layout->addWidget(title_);
    auto* columns = new QHBoxLayout;
    auto* available = new QVBoxLayout;
    auto* chosen = new QVBoxLayout;
    columns->addLayout(available, 1);
    columns->addLayout(chosen, 1);
    layout->addLayout(columns, 1);
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("groupContactSearch"));
    style_search_field(*search_);
    search_->setPlaceholderText(QStringLiteral("搜索好友"));
    available->addWidget(search_);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("groupContactPicker"));
    list_->setIconSize(QSize(32, 32));
    list_->setAccessibleName(QStringLiteral("选择群成员"));
    available->addWidget(list_, 1);
    selected_count_ = new QLabel(this);
    chosen->addWidget(selected_count_);
    selected_ = new QListWidget(this);
    selected_->setObjectName(QStringLiteral("groupSelectedContacts"));
    selected_->setTextElideMode(Qt::ElideRight);
    selected_->setAccessibleName(QStringLiteral("已选成员，点击移除"));
    chosen->addWidget(selected_, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    proceed_ = buttons->button(QDialogButtonBox::Ok);
    proceed_->setObjectName(QStringLiteral("groupCreateButton"));
    proceed_->setProperty("primary", true);
    proceed_->setText(QStringLiteral("创建群聊"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(title_, &QLineEdit::textChanged, this, &create_group_dialog::update_selection);
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
        chip->setSizeHint(QSize(0, 40));
        ++count;
    }
    selected_count_->setText(QStringLiteral("已选 %1 位好友 · 点击姓名取消").arg(count));
    proceed_->setEnabled(count > 0 && chat::valid_group_title(title_->text().toUtf8().toStdString()));
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
            auto* item = new QListWidgetItem(avatar_icon(user->username, 32, avatars.image(user->id)), user->display_name(), list_);
            item->setData(Qt::UserRole, user->id);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(chosen.contains(user->id) ? Qt::Checked : Qt::Unchecked);
            auto search = contacts.index(row, 0).data(user_model::search_role).toString();
            item->setData(user_model::search_role, search);
            item->setToolTip(user->username);
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
