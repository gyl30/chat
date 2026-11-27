#include "chat_widget.hpp"
#include "empty_state.hpp"
#include "conversation_model.hpp"
#include "message_model.hpp"
#include "theme.hpp"
#include "user_model.hpp"
#include <QButtonGroup>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSet>
#include <algorithm>
#include <QVBoxLayout>

void chat_widget::choose_forward_target(QList<QPersistentModelIndex> const& rows)
{
    if (!connection_available_ || rows.isEmpty())
    {
        return;
    }
    const auto owner = self_user_;
    const auto visit = conversation_visit_;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("选择转发目标"));
    auto* layout = new QVBoxLayout(&dialog);
    frame_dialog(dialog);
    dialog.resize(428, 480);
    auto* search = new QLineEdit;
    search->setObjectName(QStringLiteral("forwardSearch"));
    style_search_field(*search);
    search->setPlaceholderText(QStringLiteral("搜索好友或群聊"));
    layout->addWidget(search);
    auto* targets = new QListWidget;
    targets->setObjectName(QStringLiteral("forwardTargets"));
    targets->setIconSize(QSize(34, 34));
    targets->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(targets, 1);
    auto* empty = new empty_state(*targets->viewport());
    auto* preview = new QLabel;
    preview->setObjectName(QStringLiteral("forwardPreview"));
    preview->setTextFormat(Qt::PlainText);
    preview->setWordWrap(true);
    layout->addWidget(preview);
    QPushButton* bundle = nullptr;
    if (rows.size() >= 2 && rows.size() <= 50)
    {
        auto* modes = new QHBoxLayout;
        modes->setSpacing(0);
        auto* options = new QButtonGroup(&dialog);
        auto* individual = new QPushButton(QStringLiteral("逐条转发"));
        bundle = new QPushButton(QStringLiteral("合并转发"));
        bundle->setObjectName(QStringLiteral("bundleMode"));
        for (auto* choice : {individual, bundle})
        {
            choice->setAutoDefault(false);
            choice->setCheckable(true);
            choice->setProperty("forwardMode", true);
            options->addButton(choice);
            modes->addWidget(choice);
        }
        individual->setChecked(true);
        layout->addLayout(modes);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto* send = buttons->button(QDialogButtonBox::Ok);
    send->setText(QStringLiteral("确认转发"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);

    auto source_ids = [&]
    {
        QList<qint64> ids;
        for (auto const& ix : rows)
        {
            if (ix.isValid() && ix.flags().testFlag(Qt::ItemIsSelectable) &&
                ix.data(message_model::content_role).toString() == QStringLiteral("message"))
            {
                ids.append(ix.data(message_model::id_role).toLongLong());
            }
        }
        return ids;
    };
    auto update = [&]
    {
        auto ids = source_ids();
        auto* target = targets->currentItem();
        empty->set_message(targets->count()                     ? QString{}
                           : search->text().trimmed().isEmpty() ? QStringLiteral("暂无可转发的会话")
                                                                : QStringLiteral("未找到转发目标"),
                           u"search");
        send->setVisible(targets->count() != 0);
        bool complete = !ids.isEmpty() && (!bundle || !bundle->isChecked() || ids.size() == rows.size());
        send->setEnabled(connection_available_ && target && complete && owner == self_user_ && visit == conversation_visit_);
        if (!target)
        {
            preview->hide();
            return;
        }
        QString excerpt;
        if (!ids.isEmpty())
        {
            auto first = messages_->message_index(ids.front());
            excerpt = first.data(message_model::text_role).toString().simplified();
            excerpt = preview->fontMetrics().elidedText(excerpt, Qt::ElideRight, std::max(180, targets->viewport()->width() - 12));
        }
        preview->setText(complete ? QStringLiteral("转发给 %1 · %2 条消息\n%3").arg(target->text()).arg(ids.size()).arg(excerpt)
                                  : QStringLiteral("所选消息已不可转发"));
        preview->show();
    };
    connect(targets, &QListWidget::currentRowChanged, &dialog, update);
    if (bundle)
    {
        connect(bundle, &QPushButton::toggled, &dialog, update);
    }
    auto populate = [&]
    {
        const auto* selected = targets->currentItem();
        qint64 previous = selected ? selected->data(Qt::UserRole).toLongLong() : 0;
        qint64 previous_peer = selected ? selected->data(Qt::UserRole + 1).toLongLong() : 0;
        QSignalBlocker block(targets);
        auto scroll = targets->verticalScrollBar()->value();
        targets->clear();
        QString query = search->text().trimmed();
        auto add = [&](qint64 id, qint64 peer, QString const& name, QString const& username, QString const& nickname, qint64 avatar)
        {
            if (!name.contains(query, Qt::CaseInsensitive) && !username.contains(query, Qt::CaseInsensitive) &&
                !nickname.contains(query, Qt::CaseInsensitive))
            {
                return;
            }
            auto* item = new QListWidgetItem(avatar_icon(name, 34, avatars_.image(avatar), avatar < 0 ? -avatar : 0), name, targets);
            item->setSizeHint(QSize(0, 45));
            item->setToolTip(username);
            item->setData(Qt::UserRole, id);
            item->setData(Qt::UserRole + 1, peer);
            if ((previous && id == previous) || (previous_peer && previous_peer == peer))
            {
                targets->setCurrentItem(item);
            }
        };
        QSet<qint64> peers;
        for (auto const& room : conversations_->items())
        {
            if (!room.group)
            {
                peers.insert(room.user);
            }
            if (!room.speech_blocked())
            {
                add(room.id, room.group ? 0 : room.user, room.display_name(), room.username, room.nickname, room.avatar_key());
            }
        }
        for (int i = 0; i < contacts_->rowCount(); ++i)
        {
            auto* user = contacts_->user_at(contacts_->index(i));
            if (!peers.contains(user->id))
            {
                add(0, user->id, user->display_name(), user->username, user->nickname, user->id);
            }
        }
        targets->verticalScrollBar()->setValue(scroll);
        update();
    };
    connect(search, &QLineEdit::textChanged, &dialog, populate);
    auto* choose_first = new QShortcut(search);
    choose_first->setKeys({QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)});
    choose_first->setContext(Qt::WidgetShortcut);
    connect(choose_first,
            &QShortcut::activated,
            &dialog,
            [&]
            {
                if (targets->count() > 0)
                {
                    targets->setCurrentRow(0);
                    targets->setFocus();
                }
            });
    connect(conversations_, &QAbstractItemModel::modelReset, &dialog, populate);
    connect(contacts_, &QAbstractItemModel::modelReset, &dialog, populate);
    connect(&avatars_, &avatar_cache::changed, &dialog, populate);
    connect(messages_, &QAbstractItemModel::dataChanged, &dialog, update);
    connect(messages_, &QAbstractItemModel::rowsRemoved, &dialog, update);
    connect(messages_, &QAbstractItemModel::modelReset, &dialog, update);
    connect(this, &chat_widget::conversation_selected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(this, &chat_widget::friendship_updated, &dialog, update);
    populate();
    search->setFocus();
    if (dialog.exec() != QDialog::Accepted || !connection_available_ || self_user_ != owner || visit != conversation_visit_)
    {
        return;
    }
    auto* target = targets->currentItem();
    auto messages = source_ids();
    bool merged = bundle && bundle->isChecked();
    if (!target || messages.isEmpty() || (merged && messages.size() != rows.size()))
    {
        return;
    }
    auto id = target->data(Qt::UserRole).toLongLong();
    if (!id)
    {
        auto peer = target->data(Qt::UserRole + 1).toLongLong();
        if (!is_contact(peer))
        {
            return;
        }
        emit forward_to_contact_requested(peer, std::move(messages), merged);
    }
    else
    {
        auto room = conversation(id);
        if (!room || room->speech_blocked())
        {
            return;
        }
        if (merged)
        {
            emit bundle_requested(id, std::move(messages));
        }
        else
        {
            emit forward_requested(id, std::move(messages));
        }
    }
    set_message_selection(false);
}
