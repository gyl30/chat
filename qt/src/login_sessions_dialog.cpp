#include "login_sessions_dialog.hpp"

#include <QDateTime>
#include <algorithm>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QMenu>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QVBoxLayout>

#include "client_bridge.hpp"
#include "theme.hpp"
#include "theme_manager.hpp"
#include "icons.hpp"

namespace
{
enum device_role
{
    id_role = Qt::UserRole,
    current_role,
    client_role,
    created_role,
    active_role,
};

class device_delegate final : public QStyledItemDelegate
{
   public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(QStyleOptionViewItem const&, QModelIndex const& index) const override { return {0, index.data(id_role).isValid() ? 58 : 34}; }

    void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override
    {
        if (!index.data(id_role).isValid())
        {
            painter->save();
            painter->setPen(themed("text_secondary"));
            painter->drawText(option.rect.adjusted(10, 0, -10, 0), Qt::AlignVCenter, index.data().toString());
            painter->restore();
            return;
        }
        if (option.state & (QStyle::State_Selected | QStyle::State_MouseOver))
        {
            painter->fillRect(option.rect, themed(option.state & QStyle::State_Selected ? "selected" : "canvas_hover"));
        }
        const auto details = index.data(client_role).toString() + QStringLiteral(" · 最近活动 ") + index.data(active_role).toString();
        painter->save();
        auto const icon = QRect(option.rect.left() + 12, option.rect.center().y() - 12, 24, 24);
        svg_icon(u"monitor", themed("accent"), icon.size()).paint(painter, icon);
        auto const left = icon.right() + 12;
        auto const width = option.rect.right() - left - 8;
        auto font = option.font;
        font.setPixelSize(15);
        font.setBold(true);
        painter->setFont(font);
        painter->setPen(themed("text"));
        painter->drawText(QRect(left, option.rect.top() + 7, width, 24),
                          Qt::AlignVCenter,
                          QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideRight, width));
        font.setBold(false);
        font.setPixelSize(13);
        painter->setFont(font);
        painter->setPen(themed("text_secondary"));
        painter->drawText(
            QRect(left, option.rect.top() + 31, width, 19), Qt::AlignVCenter, QFontMetrics(font).elidedText(details, Qt::ElideRight, width));
        painter->restore();
    }
};
}    // namespace

login_sessions_dialog::login_sessions_dialog(client_bridge& client, QWidget* parent) : QDialog(parent), client_(client)
{
    setObjectName(QStringLiteral("loginSessionsDialog"));
    setWindowTitle(QStringLiteral("账号安全与登录设备"));
    resize(550, 470);
    auto* layout = new QVBoxLayout(this);
    frame_dialog(*this);
    sessions_ = new QListWidget(this);
    sessions_->setObjectName(QStringLiteral("loginSessionsList"));
    sessions_->setItemDelegate(new device_delegate(sessions_));
    sessions_->setMouseTracking(true);
    sessions_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sessions_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(sessions_, 1);

    auto button = [this](QString title, QString name)
    {
        auto* result = new QPushButton(std::move(title), this);
        result->setObjectName(std::move(name));
        result->setAutoDefault(false);
        return result;
    };
    auto* actions = new QHBoxLayout;
    password_entry_ = button(QStringLiteral("修改密码"), QStringLiteral("openLoginPassword"));
    revoke_others_ = button(QStringLiteral("退出其他所有设备"), QStringLiteral("revokeOtherLoginSessions"));
    auto* reload = button(QStringLiteral("刷新"), QStringLiteral("refreshLoginSessions"));
    actions->addWidget(password_entry_);
    actions->addWidget(revoke_others_);
    actions->addStretch();
    actions->addWidget(reload);
    layout->addLayout(actions);

    password_dialog_ = new QDialog(this);
    password_dialog_->setWindowTitle(QStringLiteral("修改密码"));
    auto* password_layout = new QVBoxLayout(password_dialog_);
    frame_dialog(*password_dialog_, false);
    auto* hint = new QLabel(QStringLiteral("修改密码后，其他登录会话会退出，当前会话保持登录。"), password_dialog_);
    hint->setWordWrap(true);
    password_layout->addWidget(hint);
    auto* password_form = new QFormLayout;
    auto password_field = [&](QString label, QString name)
    {
        auto* field = new QLineEdit(password_dialog_);
        field->setObjectName(std::move(name));
        field->setEchoMode(QLineEdit::Password);
        password_form->addRow(std::move(label), field);
        connect(field, &QLineEdit::textChanged, this, [this] { update_actions(); });
        return field;
    };
    current_password_ = password_field(QStringLiteral("当前密码"), QStringLiteral("currentLoginPassword"));
    new_password_ = password_field(QStringLiteral("新密码"), QStringLiteral("newLoginPassword"));
    repeat_password_ = password_field(QStringLiteral("确认新密码"), QStringLiteral("repeatLoginPassword"));
    password_layout->addLayout(password_form);
    password_status_ = new QLabel(password_dialog_);
    password_status_->setWordWrap(true);
    password_status_->setTextFormat(Qt::PlainText);
    password_status_->hide();
    password_layout->addWidget(password_status_);
    auto* password_actions = new QDialogButtonBox(QDialogButtonBox::Cancel, password_dialog_);
    change_password_ = password_actions->addButton(QStringLiteral("修改密码"), QDialogButtonBox::AcceptRole);
    change_password_->setObjectName(QStringLiteral("changeLoginPassword"));
    change_password_->setAutoDefault(false);
    password_layout->addWidget(password_actions);
    connect(password_actions, &QDialogButtonBox::rejected, password_dialog_, &QDialog::reject);
    connect(password_dialog_,
            &QDialog::finished,
            this,
            [this]
            {
                for (auto* field : {current_password_, new_password_, repeat_password_})
                {
                    field->clear();
                }
                password_status_->hide();
            });
    connect(password_entry_,
            &QPushButton::clicked,
            this,
            [this]
            {
                current_password_->setFocus(Qt::ShortcutFocusReason);
                password_dialog_->open();
            });
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("loginSessionsStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    status_->hide();

    auto close_on_session_loss = [this]
    {
        if (auto* confirmation = findChild<QMessageBox*>(QStringLiteral("confirmationDialog")))
        {
            confirmation->reject();
        }
        password_dialog_->reject();
        reject();
    };
    connect(&client_, &client_bridge::disconnected, this, close_on_session_loss);
    connect(&client_, &client_bridge::login_revoked, this, close_on_session_loss);
    connect(&client_, &client_bridge::login_sessions_changed, this, [this] { refresh(); });
    connect(reload, &QPushButton::clicked, this, [this] { refresh(); });
    connect(sessions_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { revoke(*item); });
    connect(sessions_,
            &QListWidget::customContextMenuRequested,
            this,
            [this](QPoint position)
            {
                auto const* item = sessions_->itemAt(position);
                if (!item || !item->data(id_role).isValid() || item->data(current_role).toBool() || pending_)
                {
                    return;
                }
                auto const id = item->data(id_role);
                QMenu menu(this);
                connect(this, &QDialog::finished, &menu, &QMenu::close);
                if (auto* exit = menu.addAction(QStringLiteral("退出设备")); menu.exec(sessions_->viewport()->mapToGlobal(position)) == exit)
                {
                    for (int i = 0; i < sessions_->count(); ++i)
                    {
                        if (sessions_->item(i)->data(id_role) == id)
                        {
                            revoke(*sessions_->item(i));
                            break;
                        }
                    }
                }
            });
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
                    status_->show();
                    return;
                }
                auto const* selected = sessions_->currentItem();
                auto const selected_id = selected ? selected->data(id_role).toLongLong() : 0;
                sessions_->clear();
                auto local_time = [](qint64 milliseconds)
                { return QDateTime::fromMSecsSinceEpoch(milliseconds).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")); };
                for (const bool current : {true, false})
                {
                    bool heading_added = false;
                    for (auto const& session : sessions)
                    {
                        if (session.current != current)
                        {
                            continue;
                        }
                        if (!heading_added)
                        {
                            auto* heading = new QListWidgetItem(current ? QStringLiteral("当前设备") : QStringLiteral("其他设备"), sessions_);
                            heading->setFlags(Qt::NoItemFlags);
                            heading_added = true;
                        }
                        auto* row = new QListWidgetItem(QString::fromStdString(session.device_name), sessions_);
                        row->setData(id_role, static_cast<qint64>(session.id));
                        row->setData(current_role, current);
                        row->setData(client_role, QString::fromStdString(session.client_type));
                        row->setData(created_role, local_time(session.created_at));
                        row->setData(active_role, local_time(session.last_seen));
                        row->setToolTip(row->text() + QStringLiteral("\n首次登录 ") + local_time(session.created_at));
                        if (session.id == selected_id)
                        {
                            sessions_->setCurrentItem(row);
                        }
                    }
                }
                if (std::ranges::none_of(sessions, [](auto const& session) { return !session.current; }))
                {
                    auto* hint = new QListWidgetItem(QStringLiteral("暂无其他设备"), sessions_);
                    hint->setFlags(Qt::NoItemFlags);
                }
                update_actions();
            });
    connect(&client_,
            &client_bridge::login_management_finished,
            this,
            [this](QString const& message, bool success)
            {
                pending_ = false;
                if (password_dialog_->isVisible())
                {
                    if (success)
                    {
                        password_dialog_->accept();
                    }
                    else
                    {
                        password_status_->setText(message);
                        password_status_->setVisible(!message.isEmpty());
                    }
                }
                status_->setText(message);
                status_->setVisible(!message.isEmpty());
                update_actions();
                if (success)
                {
                    refresh();
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
                    password_status_->setText(QStringLiteral("两次输入的新密码须一致，且不超过 72 个 UTF-8 字节。"));
                    password_status_->show();
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

void login_sessions_dialog::revoke(QListWidgetItem const& item)
{
    auto const id = item.data(id_role).toLongLong();
    if (!id || item.data(current_role).toBool() || pending_)
    {
        return;
    }
    if (confirm_action(this, QStringLiteral("退出设备"), QStringLiteral("退出「%1」的登录会话？").arg(item.text()), QStringLiteral("退出设备")))
    {
        pending_ = true;
        update_actions();
        client_.revoke_login_session(id);
    }
}

void login_sessions_dialog::update_actions()
{
    bool has_other = false;
    for (int i = 0; i < sessions_->count(); ++i)
    {
        auto const* item = sessions_->item(i);
        has_other |= item->data(id_role).isValid() && !item->data(current_role).toBool();
    }
    revoke_others_->setVisible(has_other);
    revoke_others_->setEnabled(!pending_ && has_other);
    password_entry_->setEnabled(!pending_);
    change_password_->setEnabled(!pending_ && !current_password_->text().isEmpty() && !new_password_->text().isEmpty() &&
                                 !repeat_password_->text().isEmpty());
}
