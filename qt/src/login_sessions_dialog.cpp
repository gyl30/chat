#include "login_sessions_dialog.hpp"

#include <QDateTime>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "client_bridge.hpp"
#include "theme.hpp"

login_sessions_dialog::login_sessions_dialog(client_bridge& client, QWidget* parent) : QDialog(parent), client_(client)
{
    setObjectName(QStringLiteral("loginSessionsDialog"));
    setWindowTitle(QStringLiteral("账号安全与登录设备"));
    resize(760, 500);
    auto* layout = new QVBoxLayout(this);
    sessions_ = new QTreeWidget(this);
    sessions_->setObjectName(QStringLiteral("loginSessionsList"));
    sessions_->setRootIsDecorated(false);
    sessions_->setHeaderLabels(
        {QStringLiteral("设备"), QStringLiteral("客户端"), QStringLiteral("首次登录"), QStringLiteral("最近活动"), QStringLiteral("状态")});
    sessions_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(sessions_, 1);

    auto button = [this](QString title, QString name)
    {
        auto* result = new QPushButton(std::move(title), this);
        result->setObjectName(std::move(name));
        result->setAutoDefault(false);
        return result;
    };
    auto* actions = new QHBoxLayout;
    revoke_ = button(QStringLiteral("退出所选设备"), QStringLiteral("revokeLoginSession"));
    revoke_others_ = button(QStringLiteral("退出其他所有设备"), QStringLiteral("revokeOtherLoginSessions"));
    auto* reload = button(QStringLiteral("刷新"), QStringLiteral("refreshLoginSessions"));
    actions->addWidget(revoke_);
    actions->addWidget(revoke_others_);
    actions->addStretch();
    actions->addWidget(reload);
    layout->addLayout(actions);

    auto* hint = new QLabel(QStringLiteral("修改密码后，其他登录会话会退出，当前会话保持登录。"), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* password_form = new QFormLayout;
    auto password_field = [&](QString label, QString name)
    {
        auto* field = new QLineEdit(this);
        field->setObjectName(std::move(name));
        field->setEchoMode(QLineEdit::Password);
        password_form->addRow(std::move(label), field);
        connect(field, &QLineEdit::textChanged, this, [this] { update_actions(); });
        return field;
    };
    current_password_ = password_field(QStringLiteral("当前密码"), QStringLiteral("currentLoginPassword"));
    new_password_ = password_field(QStringLiteral("新密码"), QStringLiteral("newLoginPassword"));
    repeat_password_ = password_field(QStringLiteral("确认新密码"), QStringLiteral("repeatLoginPassword"));
    change_password_ = button(QStringLiteral("修改密码"), QStringLiteral("changeLoginPassword"));
    password_form->addRow(QString{}, change_password_);
    layout->addLayout(password_form);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("loginSessionsStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    layout->addWidget(status_);

    auto close_on_session_loss = [this]
    {
        if (auto* confirmation = findChild<QMessageBox*>(QStringLiteral("confirmationDialog")))
        {
            confirmation->reject();
        }
        reject();
    };
    connect(&client_, &client_bridge::disconnected, this, close_on_session_loss);
    connect(&client_, &client_bridge::login_revoked, this, close_on_session_loss);
    connect(&client_, &client_bridge::login_sessions_changed, this, [this] { refresh(); });
    connect(reload, &QPushButton::clicked, this, [this] { refresh(); });
    connect(sessions_, &QTreeWidget::itemSelectionChanged, this, [this] { update_actions(); });
    connect(&client_,
            &client_bridge::login_sessions_received,
            this,
            [this](quint64 request, std::vector<chat::login_session> sessions, QString const& error)
            {
                if (request != request_)
                {
                    return;
                }
                if (!error.isEmpty())
                {
                    status_->setText(error);
                    return;
                }
                auto const* selected = sessions_->currentItem();
                auto const selected_id = selected ? selected->data(0, Qt::UserRole).toLongLong() : 0;
                sessions_->clear();
                auto local_time = [](qint64 milliseconds)
                { return QDateTime::fromMSecsSinceEpoch(milliseconds).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")); };
                for (auto const& session : sessions)
                {
                    auto* row = new QTreeWidgetItem(sessions_,
                                                    {QString::fromStdString(session.device_name),
                                                     QString::fromStdString(session.client_type),
                                                     local_time(session.created_at),
                                                     local_time(session.last_seen),
                                                     session.current ? QStringLiteral("当前会话") : QStringLiteral("其他会话")});
                    row->setData(0, Qt::UserRole, static_cast<qint64>(session.id));
                    row->setData(0, Qt::UserRole + 1, session.current);
                    if (session.id == selected_id)
                    {
                        sessions_->setCurrentItem(row);
                    }
                }
                update_actions();
            });
    connect(&client_,
            &client_bridge::login_management_finished,
            this,
            [this](QString const& message, bool success)
            {
                pending_ = false;
                status_->setText(message);
                update_actions();
                if (success)
                {
                    refresh();
                }
            });

    connect(
        revoke_,
        &QPushButton::clicked,
        this,
        [this]
        {
            auto const* selected = sessions_->currentItem();
            if (!selected)
            {
                return;
            }
            auto const id = selected->data(0, Qt::UserRole).toLongLong();
            if (confirm_action(
                    this, QStringLiteral("退出设备"), QStringLiteral("退出「%1」的登录会话？").arg(selected->text(0)), QStringLiteral("退出设备")))
            {
                pending_ = true;
                update_actions();
                client_.revoke_login_session(id);
            }
        });
    connect(revoke_others_,
            &QPushButton::clicked,
            this,
            [this]
            {
                if (confirm_action(
                        this, QStringLiteral("退出其他设备"), QStringLiteral("退出所有其他会话，保留当前会话？"), QStringLiteral("退出其他设备")))
                {
                    pending_ = true;
                    update_actions();
                    client_.revoke_other_login_sessions();
                }
            });
    connect(change_password_,
            &QPushButton::clicked,
            this,
            [this]
            {
                if (new_password_->text() != repeat_password_->text() || new_password_->text().toUtf8().size() > 72)
                {
                    status_->setText(QStringLiteral("两次输入的新密码须一致，且不超过 72 个 UTF-8 字节。"));
                    return;
                }
                auto const current = current_password_->text();
                auto const replacement = new_password_->text();
                current_password_->clear();
                new_password_->clear();
                repeat_password_->clear();
                pending_ = true;
                update_actions();
                client_.change_password(current, replacement);
            });
    update_actions();
    refresh();
}

void login_sessions_dialog::refresh() { request_ = client_.get_login_sessions(); }

void login_sessions_dialog::update_actions()
{
    auto const* selected = sessions_->currentItem();
    revoke_->setEnabled(!pending_ && selected && !selected->data(0, Qt::UserRole + 1).toBool());
    revoke_others_->setEnabled(!pending_ && sessions_->topLevelItemCount() > 1);
    change_password_->setEnabled(!pending_ && !current_password_->text().isEmpty() && !new_password_->text().isEmpty() &&
                                 !repeat_password_->text().isEmpty());
}
