#include "group_dialog.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

group_dialog::group_dialog(qint64 conversation, qint64 self_user, QString const& title, QWidget* parent)
    : QDialog(parent), conversation_(conversation), self_user_(self_user)
{
    setObjectName(QStringLiteral("groupDialog"));
    setWindowTitle(title + QStringLiteral(" · 群成员"));
    resize(480, 420);
    auto* layout = new QVBoxLayout(this);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("groupMembersList"));
    layout->addWidget(list_, 1);
    status_ = new QLabel(QStringLiteral("正在加载成员…"), this);
    status_->setObjectName(QStringLiteral("groupStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    admin_button_ = new QPushButton(QStringLiteral("设为管理员"), this);
    admin_button_->setObjectName(QStringLiteral("groupAdminButton"));
    admin_button_->setAutoDefault(false);
    layout->addWidget(admin_button_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(list_, &QListWidget::currentRowChanged, this, [this] { update_actions(); });
    connect(admin_button_, &QPushButton::clicked, this, [this] {
        auto const row = list_->currentRow();
        if (row < 0 || row >= members_.size() || !admin_button_->isEnabled())
        {
            return;
        }
        pending_ = true;
        update_actions();
        emit admin_requested(members_[row].id, members_[row].role != chat::member_role::admin);
    });
    update_actions();
}

void group_dialog::set_members(qint64 conversation, QList<member_data> members, QString const& error)
{
    if (conversation != conversation_)
    {
        return;
    }
    if (!error.isEmpty())
    {
        set_error(error);
        return;
    }
    auto const row = list_->currentRow();
    auto const selected = row >= 0 && row < members_.size() ? members_[row].id : 0;
    members_ = std::move(members);
    list_->clear();
    for (auto const& member : members_)
    {
        auto const role = member.role == chat::member_role::owner ? QStringLiteral("群主") :
            member.role == chat::member_role::admin ? QStringLiteral("管理员") : QStringLiteral("成员");
        auto* item = new QListWidgetItem(member.username + QStringLiteral(" · ") + role, list_);
        item->setData(Qt::UserRole, member.id);
        if (member.id == selected)
        {
            list_->setCurrentItem(item);
        }
    }
    available_ = true;
    status_->setText(QStringLiteral("共 %1 名成员；最多 3 名管理员，群主不计入。只有群主任免管理员。").arg(members_.size()));
    update_actions();
}

void group_dialog::finish_action(qint64 conversation, QString const& error)
{
    if (conversation != conversation_)
    {
        return;
    }
    pending_ = false;
    if (!error.isEmpty())
    {
        status_->setText(error);
    }
    update_actions();
}

void group_dialog::set_error(QString const& error)
{
    available_ = false;
    pending_ = false;
    status_->setText(error);
    update_actions();
}

void group_dialog::update_actions()
{
    auto const owner = std::any_of(members_.begin(), members_.end(), [this](auto const& member) {
        return member.id == self_user_ && member.role == chat::member_role::owner;
    });
    auto const admins = std::count_if(members_.begin(), members_.end(), [](auto const& member) {
        return member.role == chat::member_role::admin;
    });
    auto const row = list_->currentRow();
    auto const selected = row >= 0 && row < members_.size();
    auto const admin = selected && members_[row].role == chat::member_role::admin;
    admin_button_->setText(admin ? QStringLiteral("取消管理员") : QStringLiteral("设为管理员"));
    admin_button_->setEnabled(available_ && !pending_ && owner && selected &&
        members_[row].role != chat::member_role::owner && (admin || admins < 3));
}
