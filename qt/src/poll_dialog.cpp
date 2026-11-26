#include "poll_dialog.hpp"
#include "avatar.hpp"
#include "chat_widget.hpp"
#include "client_bridge.hpp"
#include "theme.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QToolButton>
#include <QVBoxLayout>

namespace
{
class option_handle final : public QLabel
{
   public:
    option_handle(QWidget* row, QVBoxLayout* list) : QLabel(QStringLiteral("≡"), row), list_(list)
    {
        setCursor(Qt::OpenHandCursor);
        setToolTip(QStringLiteral("拖动调整选项顺序"));
        setMargin(5);
    }

   private:
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (e->buttons() & Qt::LeftButton)
        {
            auto y = list_->parentWidget()->mapFromGlobal(e->globalPosition().toPoint()).y();
            auto* row = parentWidget();
            int from = list_->indexOf(row);
            for (int i = 0; i < list_->count(); ++i)
            {
                auto rect = list_->itemAt(i)->geometry();
                if ((i < from && y <= rect.center().y()) || (i > from && y >= rect.center().y() && y <= rect.bottom()))
                {
                    list_->removeWidget(row);
                    list_->insertWidget(i, row);
                    break;
                }
            }
        }
    }
    QVBoxLayout* list_;
};

struct voter_section
{
    QVBoxLayout* people;
    QPushButton* more;
    quint64 request = 0;
    qint64 after = 0;
};
}    // namespace

std::optional<std::pair<QString, QStringList>> compose_poll(QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("发起群投票"));
    auto* layout = new QVBoxLayout(&dialog);
    frame_dialog(dialog);
    auto* question = new QLineEdit;
    question->setObjectName(QStringLiteral("pollQuestion"));
    question->setPlaceholderText(QStringLiteral("输入投票问题"));
    layout->addWidget(new QLabel(QStringLiteral("投票问题")));
    layout->addWidget(question);
    layout->addWidget(new QLabel(QStringLiteral("实名单选 · 投票选项")));
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* panel = new QWidget;
    auto* rows = new QVBoxLayout(panel);
    rows->setAlignment(Qt::AlignTop);
    rows->setContentsMargins(4, 4, 4, 4);
    scroll->setWidget(panel);
    layout->addWidget(scroll, 1);
    auto* add = new QPushButton(QStringLiteral("添加选项"));
    add->setObjectName(QStringLiteral("addPollOption"));
    add->setAutoDefault(false);
    layout->addWidget(add);
    auto* feedback = new QLabel;
    feedback->setObjectName(QStringLiteral("pollFeedback"));
    layout->addWidget(feedback);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto* ok = buttons->button(QDialogButtonBox::Ok);
    ok->setText(QStringLiteral("发起投票"));
    ok->setObjectName(QStringLiteral("pollSubmit"));
    ok->setAutoDefault(false);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    auto values = [&]
    {
        QStringList result;
        for (int i = 0; i < rows->count(); ++i)
        {
            result.append(rows->itemAt(i)->widget()->findChild<QLineEdit*>()->text().trimmed());
        }
        return result;
    };
    auto validate = [&]
    {
        QString error;
        auto q = question->text().trimmed();
        QSet<QString> seen;
        if (q.isEmpty())
        {
            error = QStringLiteral("请输入投票问题");
        }
        else if (q.toUcs4().size() > 256)
        {
            error = QStringLiteral("问题最多可写 256 个字符");
        }
        for (const auto& label : values())
        {
            if (!error.isEmpty())
            {
                break;
            }
            if (label.isEmpty())
            {
                error = QStringLiteral("请填写所有选项");
            }
            else if (label.toUcs4().size() > 80)
            {
                error = QStringLiteral("每个选项最多可写 80 个字符");
            }
            else if (seen.contains(label))
            {
                error = QStringLiteral("选项不能重复");
            }
            seen.insert(label);
        }
        feedback->setText(error);
        ok->setEnabled(error.isEmpty() && rows->count() >= 2);
        add->setEnabled(rows->count() < 10);
        for (int i = 0; i < rows->count(); ++i)
        {
            rows->itemAt(i)->widget()->findChild<QToolButton*>()->setEnabled(rows->count() > 2);
        }
    };
    auto append = [&]
    {
        auto* row = new QWidget(panel);
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        line->addWidget(new option_handle(row, rows));
        auto* edit = new QLineEdit(row);
        edit->setObjectName(QStringLiteral("pollOption"));
        edit->setPlaceholderText(QStringLiteral("输入选项"));
        line->addWidget(edit, 1);
        auto* remove = new QToolButton(row);
        remove->setText(QStringLiteral("×"));
        remove->setToolTip(QStringLiteral("移除选项"));
        line->addWidget(remove);
        rows->addWidget(row);
        QObject::connect(remove,
                         &QToolButton::clicked,
                         &dialog,
                         [&, row]
                         {
                             int i = rows->indexOf(row);
                             rows->removeWidget(row);
                             row->hide();
                             row->deleteLater();
                             rows->itemAt(std::min(i, rows->count() - 1))->widget()->findChild<QLineEdit*>()->setFocus();
                             validate();
                         });
        QObject::connect(edit, &QLineEdit::textChanged, &dialog, validate);
        QObject::connect(edit,
                         &QLineEdit::returnPressed,
                         &dialog,
                         [&, row, edit]
                         {
                             auto next = rows->indexOf(row) + 1;
                             if (next < rows->count())
                             {
                                 rows->itemAt(next)->widget()->findChild<QLineEdit*>()->setFocus();
                             }
                             else if (!edit->text().trimmed().isEmpty() && add->isEnabled())
                             {
                                 add->click();
                             }
                             else if (ok->isEnabled())
                             {
                                 ok->setFocus();
                             }
                         });
        validate();
        edit->setFocus();
        scroll->ensureWidgetVisible(row);
    };
    QObject::connect(add, &QPushButton::clicked, &dialog, append);
    QObject::connect(question, &QLineEdit::textChanged, &dialog, validate);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    append();
    append();
    question->setFocus();
    dialog.resize(440, 390);
    if (dialog.exec() != QDialog::Accepted)
    {
        return std::nullopt;
    }
    return std::pair{question->text().trimmed(), values()};
}

void show_poll(client_bridge& client, chat_widget& chat, qint64 room, qint64 message, QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("投票结果"));
    auto* layout = new QVBoxLayout(&dialog);
    frame_dialog(dialog);
    auto* question = new QLabel;
    question->setWordWrap(true);
    question->setTextFormat(Qt::PlainText);
    auto font = question->font();
    font.setBold(true);
    question->setFont(font);
    layout->addWidget(question);
    auto* status = new QLabel;
    layout->addWidget(status);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    layout->addWidget(scroll);
    dialog.resize(450, 470);
    QHash<qint64, voter_section> sections;
    quint64 query = 0;
    qint64 revision = 0;
    auto reload = [&] { query = client.get_poll(room, message); };
    auto load_people = [&](qint64 choice)
    {
        auto& section = sections[choice];
        section.more->setEnabled(false);
        section.request = client.get_poll_voters(room, message, choice, section.after);
    };
    auto render = [&](const chat::poll_summary& poll)
    {
        if (poll.revision <= revision)
        {
            return;
        }
        revision = poll.revision;
        sections.clear();
        auto* body = new QWidget;
        auto* groups = new QVBoxLayout(body);
        groups->setAlignment(Qt::AlignTop);
        qint64 total = 0;
        for (auto const& option : poll.options)
        {
            total += option.votes;
        }
        status->setText(QStringLiteral("%1 · 共 %2 票").arg(poll.closed ? QStringLiteral("投票已结束") : QStringLiteral("实名单选")).arg(total));
        for (auto const& option : poll.options)
        {
            auto* heading = new QLabel(QStringLiteral("%1%   %2   ·   %3 票")
                                           .arg(total ? qRound(100. * option.votes / total) : 0)
                                           .arg(QString::fromStdString(option.label))
                                           .arg(option.votes));
            heading->setObjectName(QStringLiteral("pollGroup"));
            heading->setFont(font);
            heading->setTextFormat(Qt::PlainText);
            heading->setWordWrap(true);
            groups->addWidget(heading);
            auto* people = new QVBoxLayout;
            groups->addLayout(people);
            auto* more = new QPushButton(QStringLiteral("更多投票成员"));
            more->setVisible(false);
            more->setProperty("choice", qint64(option.position));
            groups->addWidget(more);
            sections.insert(option.position, {people, more});
            QObject::connect(more, &QPushButton::clicked, &dialog, [&, choice = option.position] { load_people(choice); });
            if (option.votes)
            {
                load_people(option.position);
            }
            groups->addSpacing(8);
        }
        int position = scroll->verticalScrollBar()->value();
        scroll->setWidget(body);
        scroll->verticalScrollBar()->setValue(position);
    };
    QObject::connect(&client,
                     &client_bridge::poll_received,
                     &dialog,
                     [&](quint64 request, qint64 r, qint64 id, chat::poll_state state, QString error)
                     {
                         if (r != room || id != message || request != query)
                         {
                             return;
                         }
                         query = 0;
                         if (!error.isEmpty())
                         {
                             status->setText(error);
                             return;
                         }
                         question->setText(QString::fromStdString(state.question));
                         render(state.summary);
                     });
    QObject::connect(&client,
                     &client_bridge::poll_changed,
                     &dialog,
                     [&](chat::poll_update update)
                     {
                         if (update.conversation == room && update.message == message)
                         {
                             render(update.summary);
                         }
                     });
    QObject::connect(&client,
                     &client_bridge::voters_received,
                     &dialog,
                     [&](quint64 req, qint64 id, qint64 choice, qint64 after, chat::poll_voters page, QString error)
                     {
                         auto it = sections.find(choice);
                         if (id != message || it == sections.end() || req != it->request || after != it->after)
                         {
                             return;
                         }
                         it->request = 0;
                         it->more->setEnabled(true);
                         if (!error.isEmpty())
                         {
                             it->more->setText(QStringLiteral("重试加载成员"));
                             it->more->show();
                             return;
                         }
                         for (auto const& member : page.members)
                         {
                             auto name = QString::fromStdString(member.nickname.empty() ? member.username : member.nickname);
                             auto* row = new QHBoxLayout;
                             auto* avatar = new QLabel;
                             chat.avatars().observe(member.id, member.avatar);
                             auto refresh = [&, avatar, user = member.id, name]
                             { avatar->setPixmap(avatar_icon(name, 26, chat.avatars().image(user)).pixmap(26, 26)); };
                             refresh();
                             QObject::connect(&chat.avatars(),
                                              &avatar_cache::changed,
                                              avatar,
                                              [refresh, user = member.id](qint64 id)
                                              {
                                                  if (id == user)
                                                  {
                                                      refresh();
                                                  }
                                              });
                             row->addWidget(avatar);
                             auto* label = new QLabel(name);
                             label->setTextFormat(Qt::PlainText);
                             label->setObjectName(QStringLiteral("pollVoter"));
                             label->setProperty("choice", choice);
                             label->setToolTip(QString::fromStdString(member.username));
                             row->addWidget(label, 1);
                             it->people->addLayout(row);
                         }
                         it->after = page.next;
                         it->more->setText(QStringLiteral("更多投票成员"));
                         it->more->setVisible(page.next > 0);
                     });
    QObject::connect(&chat,
                     &chat_widget::history_discarded,
                     &dialog,
                     [&](qint64 id, qint64 through)
                     {
                         if (id == room && message <= through)
                         {
                             dialog.reject();
                         }
                     });
    QObject::connect(&client,
                     &client_bridge::conversation_changed,
                     &dialog,
                     [&](qint64 id, bool removed)
                     {
                         if (id == room)
                         {
                             if (removed)
                             {
                                 dialog.reject();
                             }
                             else
                             {
                                 reload();
                             }
                         }
                     });
    QObject::connect(&client,
                     &client_bridge::message_updated,
                     &dialog,
                     [&](qint64 id, message_data m, QString)
                     {
                         if (id == room && m.id == message && m.deleted)
                         {
                             dialog.reject();
                         }
                     });
    QObject::connect(&client, &client_bridge::disconnected, &dialog, &QDialog::reject);
    reload();
    dialog.exec();
}
